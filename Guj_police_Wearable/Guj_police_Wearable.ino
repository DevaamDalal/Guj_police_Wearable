// ============================================================
// FINAL_POC  –  ESP32-S3 AMOLED  [WRIST MODE v14]
// Target    : Waveshare ESP32-S3-Touch-AMOLED-1.43
// Sensors   : MAX30102 (Ext) | QMI8658 (Onboard) | MLX90614 (Ext)
// BLE       : Heart-Rate, SpO2, Temp, IMU (compact notify)
//
// Step 1: Base Migration to ESP32-S3 and QMI8658 IMU integration
// ============================================================

#include <Wire.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ui_bsp.h"
#include "touch_bsp.h"
#include "rtc_bsp.h"
#include "sdlog_bsp.h"
#include <time.h>
#include "esp_heap_caps.h"

// Device name seen by the app (it registers this name on the server). Keep the SVNIT_CRL_ prefix; change the number per watch.
#define BLE_DEVICE_NAME "SVNIT_CRL_60"
#define FW_LABEL        "stage 6 v0 (BLE)"
#define USE_BLE 1   // 1 = BLE on (stage 5). 0 = local only. BLE needs the 16M partition scheme (see README).
#if USE_BLE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#endif

// ─────────────────────── PINS FOR ESP32-S3 AMOLED ───────────────────
// According to Waveshare schematic, onboard peripherals (Touch, IMU, RTC) 
// and the 4-pin SH1.0 expansion connector share the same I2C pins.
#define SDA_PIN        47
#define SCL_PIN        48

#define QMI_INT_PIN    8  // Built-in IMU interrupt pin
#define MAX_INT_PIN    15 // Expansion pin for MAX30102 Interrupt (adjust as wired)

// ─────────────────────── I2C ADDRESSES ──────────────────────
#define MAX30102_ADDR  0x57
#define QMI8658_ADDR   0x6B // Default I2C address for onboard QMI8658
#define MLX90614_ADDR  0x5A

#if USE_BLE
// ─────────────────────── BLE UUIDs ──────────────────────────
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLECharacteristic *pCharacteristic;
bool deviceConnected = false;

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) { deviceConnected = true; Serial.println("BLE: client connected"); }
  void onDisconnect(BLEServer *pServer) {
    deviceConnected = false;
    Serial.println("BLE: client disconnected, advertising again");
    BLEDevice::startAdvertising();
  }
};
#endif

TwoWire I2Cbus = TwoWire(0);

// One mutex for the shared I2C bus: sensors (loop task) and touch (LVGL task).
SemaphoreHandle_t g_i2cMux = NULL;
static inline void i2cLock()   { xSemaphoreTake(g_i2cMux, portMAX_DELAY); }
static inline void i2cUnlock() { xSemaphoreGive(g_i2cMux); }

// ─────────────────────── INTERRUPT FLAGS ────────────────────
volatile bool imuDataReady = false;
volatile bool maxDataReady = false;
void IRAM_ATTR onIMUInterrupt() { imuDataReady = true; }
void IRAM_ATTR onMAXInterrupt() { maxDataReady = true; }

// ───────────── MAX30102 new-sample gate ─────────────
// FIFO diagnostic result (2026-10-13): loop() ran ~195 passes/s but the sensor delivers ~25 samples/s, and 90 % of the
// FIFO reads were EMPTY reads (stale data), all fed into the HR/SpO2 algorithm. The v14 firmware avoided this with the
// MAX30102 interrupt flag (one pass per sample); the S3 port polls without interrupt. This gate restores that
// behaviour: read the FIFO pointers and run the algorithm only when a NEW sample is waiting. Set to 0 to go back.
#define MAX_GATE_NEW_SAMPLE 1

// ─────────────────────── SIGNAL CONSTANTS ───────────────────
const float    DC_ALPHA                    = 0.985f;
const uint32_t MIN_SKIN_IR                 = 6000;
const uint32_t MAX_VALID_ADC               = 250000;
const float    MAX_RAW_CHANGE              = 6000.0f;
const int      MAX_BAD_MOTION_STREAK       = 5;
const int      POST_MOTION_WARMUP_SAMPLES  = 15;
const int      WARMUP_SAMPLES              = 40;
const int      MIN_SIGNAL_AMPLITUDE        = 45;

const float    MIN_SPO2_AC_DC_RATIO        = 0.00035f;
const float    MAX_SPO2_AC_DC_RATIO        = 0.20f;

const float    GYRO_MOTION_THRESH          = 45.0f;
const int      GYRO_MOTION_CONSEC          = 8;
const int      SPO2_MOTION_FLUSH_AT        = 35;

const uint32_t MIN_RR_MS                   = 650;
const uint32_t HR_TIMEOUT_MS               = 8000;
const uint32_t HR_HOLD_AFTER_IMU_MS        = 5000;

const float    WRIST_R_MIN                 = 0.40f;
const float    WRIST_R_MAX                 = 1.30f;

const float    SPO2_COEFF_A                = 112.5f;
const float    SPO2_COEFF_B                = 25.0f;

// Counted in algorithm passes. Old: 18 passes (about 0.1 s of stale-repeated data). With the gate each pass is one real
// sample (25/s), so 50 samples = 2 s, about 2-3 beats. Needs on-wrist tuning later.
const int      SPO2_MIN_WINDOW_SAMPLES     = MAX_GATE_NEW_SAMPLE ? 50 : 18;
const int      SPO2_RESET_WINDOW_SAMPLES   = 150;
const float    SPO2_MAX_R_CHANGE           = 0.28f;
const int      SPO2_OUTLIER_HOLD_WINDOWS   = 6;

// ─────────────────────── DC TRACKING ────────────────────────
float irDC = 0, redDC = 0;
float prevIrDC = 0, prevRedDC = 0;
bool dcInitialized = false;

// ─────────────────────── SIGNAL STATE ───────────────────────
uint32_t prevAdcIR = 0, prevAdcRed = 0;
bool hasPrevSample = false;
bool isSignalValid = false;
int sampleCounter = 0;
int motionStreak = 0;
uint32_t lastStatusMs = 0;
char statusMsg[24] = "WARMUP";

// ─────────────────────── HEART RATE ─────────────────────────
float irPrev = 0, irPrevPrev = 0;
bool rising = false;
uint32_t lastBeatMs = 0;
uint32_t beatIntervals[8];
int beatCount = 0;
float heartRate = 0;
bool hrValid = false;
int inconsistentBeats = 0;

float hrSmoothed = 0.0f;
int hrStableCount = 0;
const int HR_STABLE_REQUIRED = 2;
const float HR_EWMA_ALPHA = 0.25f;
const float HR_REL_TOLERANCE = 0.08f;

float lastDisplayedHR = 0.0f;
uint32_t lastGoodHRMs = 0;

// ─────────────────────── SPO2 ───────────────────────────────
float sumIR_sq = 0, sumRed_sq = 0;
float sumIR = 0, sumRed = 0;
int spo2Count = 0;
float spO2 = 0;
bool spo2Valid = false;
float prevR = 0;
int rOutliers = 0;

float spO2Smoothed = 0.0f;
int spo2StableCount = 0;
const int SPO2_STABLE_REQUIRED = 2;
const float SPO2_EWMA_ALPHA = 0.12f;
const float SPO2_REL_TOLERANCE = 0.04f;

// ─────────────────────── QUALITY ────────────────────────────
int qualityScore = 0;

// ─────────────────────── IMU (QMI8658) ──────────────────────
float accX = 0, accY = 0, accZ = 0;
float gyroX = 0, gyroY = 0, gyroZ = 0;
bool gyroMotion = false;
int gyroHighCount = 0;
int imuMotionStreak = 0;

float accelMagRaw = 0.0f;
float accelMagFilt = 0.0f;
const float ACCEL_EWMA_ALPHA = 0.15f;

float gyroMagFilt = 0.0f;
const float GYRO_EWMA_ALPHA = 0.15f;

// ─────────────────────── MLX90614 ───────────────────────────
float mlxObjectTemp = 0.0f;
float mlxAmbientTemp = 0.0f;
float mlxFiltered = 0.0f;
bool mlxInitialized = false;
unsigned long lastTempRead = 0;

// ─────────────────────── PRINT TIMING ───────────────────────
unsigned long lastPrint = 0;

// ─────────────────────── LOGGING / RTC / BATTERY STATE ───────────────────────
const uint32_t LOG_PERIOD_MS = 100;            // SD log rate: 100 ms = 10 rows/s (~10 MB/hour)
uint32_t lastIR = 0, lastRed = 0;              // latest raw FIFO values (for the logger)
float lastIrAC = 0, lastRedAC = 0;
char rtcStr[24] = "RTC invalid";
RtcTime rtcNow = {2000, 1, 1, 0, 0, 0};          // last RTC reading (for the watch face)
bool rtcOk = false;
float battV = 0;
int battPct = -1;

void writeReg(uint8_t reg, uint8_t val) {
  I2Cbus.beginTransmission(MAX30102_ADDR);
  I2Cbus.write(reg);
  I2Cbus.write(val);
  I2Cbus.endTransmission();
}

uint8_t readReg(uint8_t reg) {
  I2Cbus.beginTransmission(MAX30102_ADDR);
  I2Cbus.write(reg);
  I2Cbus.endTransmission(false);
  I2Cbus.requestFrom(MAX30102_ADDR, (uint8_t)1);
  return I2Cbus.read();
}

uint32_t getMedianInterval(uint32_t arr[], int count) {
  if (count <= 0) return 0;
  uint32_t tmp[8];
  for (int i = 0; i < count; i++) tmp[i] = arr[i];
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - i - 1; j++) {
      if (tmp[j] > tmp[j + 1]) {
        uint32_t t = tmp[j];
        tmp[j] = tmp[j + 1];
        tmp[j + 1] = t;
      }
    }
  }
  return tmp[count / 2];
}

bool hasContact(uint32_t adcIR, uint32_t adcRed) {
  return adcIR > MIN_SKIN_IR && adcRed > MIN_SKIN_IR &&
         adcIR < MAX_VALID_ADC && adcRed < MAX_VALID_ADC;
}

bool detectMotionArtifact(uint32_t adcIR, uint32_t adcRed) {
  if (!hasPrevSample || sampleCounter < WARMUP_SAMPLES) return false;

  float irRawChange = fabsf((float)adcIR - (float)prevAdcIR);
  float redRawChange = fabsf((float)adcRed - (float)prevAdcRed);
  float irDcChange = fabsf(irDC - prevIrDC);
  float redDcChange = fabsf(redDC - prevRedDC);

  float dcThresh = MAX_RAW_CHANGE * (1.0f - DC_ALPHA) * 2.0f;

  return irRawChange > MAX_RAW_CHANGE ||
         redRawChange > MAX_RAW_CHANGE ||
         irDcChange > dcThresh ||
         redDcChange > dcThresh;
}

void setStatus(const char *msg) {
  strncpy(statusMsg, msg, sizeof(statusMsg) - 1);
  statusMsg[sizeof(statusMsg) - 1] = '\0';
}

void printStatusThrottled(const char *msg) {
  setStatus(msg);
  uint32_t now = millis();
  if (now - lastStatusMs >= 600) {
    Serial.println(msg);
    lastStatusMs = now;
  }
}

void resetDerivedReadings() {
  irPrev = irPrevPrev = 0;
  rising = false;
  lastBeatMs = 0;
  beatCount = 0;
  heartRate = 0;
  hrValid = false;
  inconsistentBeats = 0;
  hrSmoothed = 0.0f;
  hrStableCount = 0;
  lastDisplayedHR = 0.0f;
  lastGoodHRMs = 0;

  sumIR_sq = sumRed_sq = sumIR = sumRed = 0;
  spo2Count = 0;
  spO2 = 0;
  spo2Valid = false;
  spO2Smoothed = 0.0f;
  spo2StableCount = 0;
  prevR = 0;
  rOutliers = 0;

  dcInitialized = false;
}

void startPostMotionRecovery(uint32_t adcIR, uint32_t adcRed) {
  resetDerivedReadings();

  irDC = prevIrDC = (float)adcIR;
  redDC = prevRedDC = (float)adcRed;
  dcInitialized = true;

  prevAdcIR = adcIR;
  prevAdcRed = adcRed;
  hasPrevSample = false;
  sampleCounter = max(0, WARMUP_SAMPLES - POST_MOTION_WARMUP_SAMPLES);
  motionStreak = 0;
  setStatus("WARMUP");
}

void markSpo2Outlier() {
  rOutliers++;
  spo2StableCount = 0;
  if (spO2 <= 0 || rOutliers >= SPO2_OUTLIER_HOLD_WINDOWS) {
    spo2Valid = false;
  }
}

void initMAX() {
  writeReg(0x09, 0x40);
  delay(500);

  writeReg(0x08, 0x7F);
  writeReg(0x09, 0x03);
  writeReg(0x0A, 0x2B);

  writeReg(0x0C, 0x19);
  writeReg(0x0D, 0x19);

  writeReg(0x02, 0xC0);

  Serial.print("MAX30102 PART ID: 0x");
  Serial.println(readReg(0xFF), HEX);
}

// === NEW IMU INITIALIZATION FOR QMI8658 ===
void initQMI8658() {
  I2Cbus.beginTransmission(QMI8658_ADDR);
  uint8_t err = I2Cbus.endTransmission();
  if (err != 0) {
    Serial.println("QMI8658 not found! Check I2C pins or address.");
    return;
  }
  
  // Basic QMI8658 config for standard operation 
  // (We use simplified raw register writes to match the old MPU6050 style)
  I2Cbus.beginTransmission(QMI8658_ADDR);
  I2Cbus.write(0x60); // RESET
  I2Cbus.write(0x01); 
  I2Cbus.endTransmission();
  delay(50);
  
  I2Cbus.beginTransmission(QMI8658_ADDR);
  I2Cbus.write(0x02); // CTRL1
  I2Cbus.write(0x60); // Auto-increment, Address increment
  I2Cbus.endTransmission();

  I2Cbus.beginTransmission(QMI8658_ADDR);
  I2Cbus.write(0x03); // CTRL2 (Accel config) - 8g, 250Hz
  I2Cbus.write(0x13);
  I2Cbus.endTransmission();

  I2Cbus.beginTransmission(QMI8658_ADDR);
  I2Cbus.write(0x04); // CTRL3 (Gyro config) - 512dps, 250Hz
  I2Cbus.write(0x53); 
  I2Cbus.endTransmission();

  I2Cbus.beginTransmission(QMI8658_ADDR);
  I2Cbus.write(0x08); // CTRL7 - Enable Gyro & Accel
  I2Cbus.write(0x03); 
  I2Cbus.endTransmission();
  
  Serial.println("QMI8658 OK");
}

bool initMLX90614() {
  I2Cbus.beginTransmission(MLX90614_ADDR);
  uint8_t err = I2Cbus.endTransmission();
  if (err != 0) {
    Serial.print("MLX90614 not found! err=");
    Serial.println(err);
    return false;
  }
  Serial.println("MLX90614 OK");
  return true;
}

static float mlxReadCelsius(uint8_t ramAddr) {
  I2Cbus.beginTransmission(MLX90614_ADDR);
  I2Cbus.write(ramAddr);
  uint8_t err = I2Cbus.endTransmission(false);
  if (err != 0) return NAN;

  uint8_t count = I2Cbus.requestFrom((uint8_t)MLX90614_ADDR, (uint8_t)3);
  if (count < 3) return NAN;

  uint8_t lsb = I2Cbus.read();
  uint8_t msb = I2Cbus.read();
  I2Cbus.read();

  if (msb & 0x80) return NAN;

  uint16_t raw = ((uint16_t)msb << 8) | lsb;
  return (raw * 0.02f) - 273.15f;
}

void updateTempMLX() {
  const unsigned long READ_INTERVAL_MS = 500;
  const float VALID_MIN_C = 20.0f, VALID_MAX_C = 43.0f;

  if (millis() - lastTempRead < READ_INTERVAL_MS) return;
  lastTempRead = millis();

  float objTemp = mlxReadCelsius(0x07);
  float ambTemp = mlxReadCelsius(0x06);

  if (!isnan(ambTemp)) mlxAmbientTemp = ambTemp;
  if (isnan(objTemp) || objTemp < VALID_MIN_C || objTemp > VALID_MAX_C) return;

  if (!mlxInitialized) {
    mlxFiltered = objTemp;
    mlxInitialized = true;
    mlxObjectTemp = objTemp;
    return;
  }

  float diff = fabsf(objTemp - mlxFiltered);
  float alpha = (diff > 1.0f) ? 0.35f : (diff > 0.4f) ? 0.20f : 0.08f;
  mlxFiltered += alpha * (objTemp - mlxFiltered);
  mlxObjectTemp = mlxFiltered;
}

void updateIMU() {
  // Read 12 bytes of Accel + Gyro from QMI8658
  I2Cbus.beginTransmission(QMI8658_ADDR);
  I2Cbus.write(0x35); // AX_L register
  I2Cbus.endTransmission(false);
  I2Cbus.requestFrom(QMI8658_ADDR, 12);

  if (I2Cbus.available() >= 12) {
    int16_t ax = I2Cbus.read() | (I2Cbus.read() << 8);
    int16_t ay = I2Cbus.read() | (I2Cbus.read() << 8);
    int16_t az = I2Cbus.read() | (I2Cbus.read() << 8);
    int16_t gx = I2Cbus.read() | (I2Cbus.read() << 8);
    int16_t gy = I2Cbus.read() | (I2Cbus.read() << 8);
    int16_t gz = I2Cbus.read() | (I2Cbus.read() << 8);

    // QMI8658 scale factors (8g = 4096 LSB/g, 512dps = 64 LSB/dps)
    accX = ax / 4096.0f;
    accY = ay / 4096.0f;
    accZ = az / 4096.0f;
    gyroX = gx / 64.0f;
    gyroY = gy / 64.0f;
    gyroZ = gz / 64.0f;
  }

  float gyroMag = sqrtf(gyroX * gyroX + gyroY * gyroY + gyroZ * gyroZ);
  if (gyroMag > GYRO_MOTION_THRESH) {
    gyroHighCount++;
    if (gyroHighCount >= GYRO_MOTION_CONSEC) gyroMotion = true;
  } else {
    gyroHighCount = 0;
    gyroMotion = false;
  }

  imuDataReady = false;

  float accelMag = sqrtf(accX * accX + accY * accY + accZ * accZ);
  accelMagRaw = accelMag;
  if (accelMagFilt <= 0.0f) accelMagFilt = accelMag;
  else accelMagFilt = (1.0f - ACCEL_EWMA_ALPHA) * accelMagFilt + ACCEL_EWMA_ALPHA * accelMag;

  if (gyroMagFilt <= 0.0f) gyroMagFilt = gyroMag;
  else gyroMagFilt = (1.0f - GYRO_EWMA_ALPHA) * gyroMagFilt + GYRO_EWMA_ALPHA * gyroMag;
}

// Push current readings to the screen (non-blocking, 4 Hz). Called every loop,
// before any early return, so NO_CONTACT/WARMUP/MOTION states also reach the display.
void uiPush() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 250) return;
  last = now;
  bool hrShow = hrValid || (lastDisplayedHR > 0 && (now - lastGoodHRMs) <= HR_HOLD_AFTER_IMU_MS);
  ui_set_values(hrValid ? heartRate : lastDisplayedHR, hrShow,
                spO2, spo2Valid && spO2 > 0,
                mlxObjectTemp, mlxAmbientTemp,
                gyroMotion ? "IMU_MOTION" : statusMsg);
  ui_set_debug(accelMagFilt, gyroMagFilt, irDC, redDC);
}

// ───────────── Battery: this board has NO fuel gauge, only a voltage divider on GPIO4 ─────────────
// Voltage-based % estimate (resting LiPo curve). Reads high while charging / USB powered.
#define BAT_ADC_PIN 4
static int battPercentFromMv(int mv) {
  static const int mvT[] = {3270,3610,3690,3710,3730,3750,3770,3790,3800,3820,3840,
                            3850,3870,3910,3950,3980,4020,4080,4110,4150,4200};
  static const int pcT[] = {   0,   5,  10,  15,  20,  25,  30,  35,  40,  45,  50,
                              55,  60,  65,  70,  75,  80,  85,  90,  95, 100};
  if (mv <= mvT[0]) return 0;
  if (mv >= mvT[20]) return 100;
  for (int i = 1; i < 21; i++)
    if (mv <= mvT[i]) return pcT[i - 1] + (pcT[i] - pcT[i - 1]) * (mv - mvT[i - 1]) / (mvT[i] - mvT[i - 1]);
  return 100;
}
static void updateBattery() {
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(BAT_ADC_PIN);
  int mv = (int)(acc / 8) * 3;                   // Waveshare demo: 1/3 divider
  battV = mv / 1000.0f;
  battPct = (mv < 2500) ? -1 : battPercentFromMv(mv);
}

// ───────────── RTC helpers ─────────────
static time_t rtcToEpoch(const RtcTime &t) {
  struct tm tmv = {};
  tmv.tm_year = t.year - 1900; tmv.tm_mon = t.month - 1; tmv.tm_mday = t.day;
  tmv.tm_hour = t.hour; tmv.tm_min = t.min; tmv.tm_sec = t.sec;
  return mktime(&tmv);
}
static void updateRtcStr() {
  RtcTime t;
  rtcOk = rtc_get(&t);
  if (rtcOk) {
    rtcNow = t;
    snprintf(rtcStr, sizeof(rtcStr), "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.min, t.sec);
  } else snprintf(rtcStr, sizeof(rtcStr), "RTC invalid");
}
// If the RTC lost power or is behind this firmware's build time, set it to the build time.
// (Exact time: send  T2026-10-10 13:20:11  in the Serial Monitor with "Newline" enabled.)
static void rtcSyncFromBuild() {
  char mon[4]; int d = 1, y = 2026, h = 0, mi = 0, s = 0;
  sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
  sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s);
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *p = strstr(months, mon);
  RtcTime b = { y, p ? (int)(p - months) / 3 + 1 : 1, d, h, mi, s };
  RtcTime now;
  if (!rtc_get(&now) || rtcToEpoch(now) < rtcToEpoch(b)) {
    Serial.println(rtc_set(&b) ? "RTC set from build time (send T<YYYY-MM-DD HH:MM:SS> to correct)" : "RTC write FAILED");
  } else {
    Serial.println("RTC valid");
  }
  updateRtcStr();
}
// ───────────── FIFO diagnostic (Serial command  F ) ─────────────
// Question it answers: loop() reads MAX30102 FIFO_DATA on every pass. If loop() runs much faster than
// the sensor produces samples (about 25/s here), the same sample may be read repeatedly (or an empty
// FIFO read), which would distort HR/SpO2. For 30 s this records, on every pass, the FIFO write pointer,
// overflow counter, read pointer and the IR/RED values read, then prints a summary and saves
// DIAGnnnn.CSV to the SD card. Keep the finger on the sensor, still, while it runs.
#define DIAG_MAX_ROWS   16000
#define DIAG_DURATION_MS 30000
struct DiagRow { uint32_t t_us, ir, red; uint8_t wr, ovf, rd, pad; };
static DiagRow *diagBuf = NULL;
static volatile bool diagActive = false;
static uint32_t diagN = 0, diagStartMs = 0;
static int32_t diagSaveIdx = -1;                 // >=0 while rows are being written to SD
static bool diagSdOk = false;

static void diagReadPtrs(uint8_t *wr, uint8_t *ovf, uint8_t *rd) {   // call with the I2C lock held
  I2Cbus.beginTransmission(MAX30102_ADDR);
  I2Cbus.write(0x04);
  I2Cbus.endTransmission(false);
  if (I2Cbus.requestFrom(MAX30102_ADDR, (uint8_t)3) == 3) {
    *wr = I2Cbus.read() & 0x1F; *ovf = I2Cbus.read() & 0x1F; *rd = I2Cbus.read() & 0x1F;
  }
}
static void diagRecord(uint8_t wr, uint8_t ovf, uint8_t rd, uint32_t ir, uint32_t red) {
  if (!diagActive || diagN >= DIAG_MAX_ROWS) return;
  DiagRow &r = diagBuf[diagN++];
  r.t_us = micros(); r.ir = ir; r.red = red; r.wr = wr; r.ovf = ovf; r.rd = rd; r.pad = 0;
}
static void diagStart() {
  if (diagActive || diagSaveIdx >= 0) { Serial.println("DIAG already running"); return; }
  if (!diagBuf) diagBuf = (DiagRow *)heap_caps_malloc(sizeof(DiagRow) * DIAG_MAX_ROWS, MALLOC_CAP_SPIRAM);
  if (!diagBuf) { Serial.println("DIAG: PSRAM allocation failed"); return; }
  diagN = 0; diagStartMs = millis(); diagActive = true;
  Serial.println("DIAG: capturing 30 s - keep the finger still on the sensor");
}
static void diagSummary() {
  if (diagN < 3) { Serial.println("DIAG: too few rows"); return; }
  uint32_t newS = 0, consumed = 0, changes = 0, processed = 0, empty = 0, maxAvail = 0, maxOvf = 0, maxGapUs = 0;
  for (uint32_t i = 0; i < diagN; i++) {
    const DiagRow &r = diagBuf[i];
    uint32_t avail = (uint32_t)((r.wr - r.rd) & 0x1F);
    if (avail == 0) empty++;
    if (avail > maxAvail) maxAvail = avail;
    if (r.ovf > maxOvf) maxOvf = r.ovf;
    if (i > 0) {
      const DiagRow &p = diagBuf[i - 1];
      newS += (uint32_t)((r.wr - p.wr) & 0x1F);
      consumed += (uint32_t)((r.rd - p.rd) & 0x1F);
      if ((r.ir | r.red) && (p.ir | p.red) && (r.ir != p.ir || r.red != p.red)) changes++;
      if (r.ir | r.red) processed++;
      if (r.t_us - p.t_us > maxGapUs) maxGapUs = r.t_us - p.t_us;
    }
  }
  float dur = (diagBuf[diagN - 1].t_us - diagBuf[0].t_us) / 1e6f;
  float passes = diagN / dur, newPs = newS / dur, consPs = consumed / dur, chgPs = changes / dur;
  Serial.println("──── FIFO DIAGNOSTIC ────");
  Serial.printf("rows=%lu  duration=%.1f s  loop passes/s=%.1f  longest gap=%.1f ms\n", (unsigned long)diagN, dur, passes, maxGapUs / 1000.0f);
  Serial.printf("new samples/s (WR ptr)=%.1f  samples consumed/s (RD ptr)=%.1f  value changes/s=%.1f\n", newPs, consPs, chgPs);
  Serial.printf("passes with empty FIFO=%.0f%%  max backlog=%lu  max overflow counter=%lu\n", 100.0f * empty / diagN, (unsigned long)maxAvail, (unsigned long)maxOvf);
  Serial.printf("passes that fed the algorithm=%.1f/s (should equal new samples/s)\n", processed / dur);
#if MAX_GATE_NEW_SAMPLE
  Serial.println("GATE ON: empty FIFO reads are skipped. Good result = fed/s about equal to new samples/s (about 25) and value changes/s about 25.");
#else
  if (passes > 1.5f * newPs && newPs > 0)
    Serial.printf("VERDICT: loop runs %.1fx faster than the sensor -> most passes re-read old data. Algorithm should be fed once per NEW sample.\n", passes / newPs);
  else if (maxAvail > 4 || maxOvf > 0)
    Serial.println("VERDICT: loop is slower than the sensor - backlog/overflow, samples are being lost or delayed.");
  else
    Serial.println("VERDICT: loop and sensor rates are matched; the FIFO read is not the cause of the noise.");
#endif
}
static void diagService() {
  if (diagActive && (millis() - diagStartMs >= DIAG_DURATION_MS || diagN >= DIAG_MAX_ROWS)) {
    diagActive = false;
    diagSummary();
    char name[16];
    diagSdOk = sdlog_aux_open("DIAG", "CSV", name, sizeof(name));
    if (diagSdOk) {
      Serial.printf("DIAG: saving %s\n", name);
      sdlog_aux_write("t_us,wr,ovf,rd,avail,ir,red\n");
      diagSaveIdx = 0;
    } else Serial.println("DIAG: no SD card, summary only");
  }
  if (diagSaveIdx >= 0) {                        // write 100 rows per pass so the sensor loop is never stalled
    char row[48], chunk[48 * 20];
    for (int k = 0; k < 2 && diagSaveIdx >= 0; k++) {
      chunk[0] = '\0';
      size_t len = 0;
      for (int j = 0; j < 20 && (uint32_t)diagSaveIdx < diagN; j++, diagSaveIdx++) {
        const DiagRow &r = diagBuf[diagSaveIdx];
        int n = snprintf(row, sizeof(row), "%lu,%u,%u,%u,%u,%lu,%lu\n", (unsigned long)r.t_us, r.wr, r.ovf, r.rd,
                         (unsigned)((r.wr - r.rd) & 0x1F), (unsigned long)r.ir, (unsigned long)r.red);
        memcpy(chunk + len, row, n); len += n; chunk[len] = '\0';
      }
      if (len) sdlog_aux_write(chunk);
      if ((uint32_t)diagSaveIdx >= diagN) { sdlog_aux_close(); diagSaveIdx = -1; Serial.println("DIAG: file saved"); }
    }
  }
}

static void pollSerialCmd() {
  static char buf[40];
  static int n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if ((c == 'F' || c == 'f') && n == 0) { diagStart(); continue; }   // single key press is enough, no Enter needed
    if (c == '\n' || c == '\r') {
      if (n > 0) {
        buf[n] = '\0';
        if (buf[0] == 'T') {
          int y, mo, d, h, mi, s;
          if (sscanf(buf + 1, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
            RtcTime t = { y, mo, d, h, mi, s };
            Serial.println(rtc_set(&t) ? "RTC set" : "RTC set FAILED");
            updateRtcStr();
          } else Serial.println("Use: T2026-10-10 13:20:11");
        } else if (buf[0] == 'F') {
          diagStart();
        }
        n = 0;
      }
    } else if (n < (int)sizeof(buf) - 1) buf[n++] = c;
  }
}

// ───────────── SD log: same key=value fields as the Serial line, plus MS / RTC / VBAT ─────────────
static void buildLogLine(char *out, size_t n) {
  uint32_t now = millis();
  bool hrShow = hrValid || (lastDisplayedHR > 0 && (now - lastGoodHRMs) <= HR_HOLD_AFTER_IMU_MS);
  char hrS[12], spS[12];
  if (hrShow) snprintf(hrS, sizeof(hrS), "%.1f", hrValid ? heartRate : lastDisplayedHR); else strcpy(hrS, "--");
  if (spo2Valid && spO2 > 0) snprintf(spS, sizeof(spS), "%.1f%%", spO2); else strcpy(spS, "--");
  snprintf(out, n,
    "MS=%lu,RTC=%s,VBAT=%.2f,FIFO_IR=%lu,FIFO_RED=%lu,S1_IR=%02X %02X %02X,S1_RED=%02X %02X %02X,"
    "IR_DC=%.1f,IR_AC=%.1f,RED_DC=%.1f,RED_AC=%.1f,HR=%s,SpO2=%s,ObjT=%.1f,AmbT=%.1f,"
    "accX=%.3f,accY=%.3f,accZ=%.3f,accMag=%.3f,accMagF=%.3f,gMagF=%.2f,gX=%.2f,gY=%.2f,gZ=%.2f,ST=%s,Q=%d",
    (unsigned long)now, rtcStr, battV, (unsigned long)lastIR, (unsigned long)lastRed,
    (unsigned)((lastIR >> 16) & 0x03), (unsigned)((lastIR >> 8) & 0xFF), (unsigned)(lastIR & 0xFF),
    (unsigned)((lastRed >> 16) & 0x03), (unsigned)((lastRed >> 8) & 0xFF), (unsigned)(lastRed & 0xFF),
    irDC, lastIrAC, redDC, lastRedAC, hrS, spS, mlxObjectTemp, mlxAmbientTemp,
    accX, accY, accZ, accelMagRaw, accelMagFilt, gyroMagFilt, gyroX, gyroY, gyroZ,
    gyroMotion ? "IMU_MOTION" : statusMsg, qualityScore);
}
static void logPush() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < LOG_PERIOD_MS) return;
  last = now;
  if (!sdlog_active()) return;
  char line[SDLOG_LINE_MAX];
  buildLogLine(line, sizeof(line));
  sdlog_write(line);
}
// ───────────── BLE packet (1 Hz) ─────────────
// App contract (CSE team): Key=Value pairs separated by commas; it reads HR, SpO2, Temp, Batt; other keys are forwarded to the
// server. "--" = no reading. Keep under ~200 characters. The SD log and Serial keep the long line.
static int bleBuildPacket(char *out, size_t n) {
  uint32_t now = millis();
  bool hrShow = hrValid || (lastDisplayedHR > 0 && (now - lastGoodHRMs) <= HR_HOLD_AFTER_IMU_MS);
  char hrS[12], spS[12], bS[8];
  if (hrShow) snprintf(hrS, sizeof(hrS), "%.1f", hrValid ? heartRate : lastDisplayedHR); else strcpy(hrS, "--");
  if (spo2Valid && spO2 > 0) snprintf(spS, sizeof(spS), "%.1f", spO2); else strcpy(spS, "--");
  if (battPct >= 0) snprintf(bS, sizeof(bS), "%d", battPct); else strcpy(bS, "--");
  return snprintf(out, n,
    "HR=%s,SpO2=%s,Temp=%.1f,Batt=%s,accX=%.3f,accY=%.3f,accZ=%.3f,gX=%.1f,gY=%.1f,gZ=%.1f,ST=%s",
    hrS, spS, mlxObjectTemp, bS, accX, accY, accZ, gyroX, gyroY, gyroZ,
    gyroMotion ? "IMU_MOTION" : statusMsg);
}
static void bleNotify() {
#if USE_BLE
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 1000) return;
  last = now;
  if (!deviceConnected) return;
  char pkt[200];
  int len = bleBuildPacket(pkt, sizeof(pkt));
  if (len <= 0) return;
  if (len >= (int)sizeof(pkt)) len = sizeof(pkt) - 1;
  pCharacteristic->setValue((uint8_t *)pkt, (size_t)len);
  pCharacteristic->notify();
  Serial.printf("BLE TX (%d chars): %s\n", len, pkt);
#endif
}

// 2 Hz: clock + battery for the watch face, text lines for the system page.
// The battery ADC is only sampled every 5 s (it changes slowly).
static void infoPush() {
  static uint32_t last = 0, lastBatt = 0;
  uint32_t now = millis();
  if (now - last < 500) return;
  last = now;
  if (lastBatt == 0 || now - lastBatt >= 5000) { updateBattery(); lastBatt = now; }
  updateRtcStr();
  char l1[40], l2[40], l3[40];
  if (battPct >= 0) snprintf(l1, sizeof(l1), "Batt %.2f V  ~%d%%", battV, battPct);
  else              snprintf(l1, sizeof(l1), "Batt --");
  sdlog_status(l2, sizeof(l2));
  snprintf(l3, sizeof(l3), "%s", rtcStr);
  ui_set_info(l1, l2, l3);
  ui_set_clock(rtcNow.year, rtcNow.month, rtcNow.day, rtcNow.hour, rtcNow.min, rtcNow.sec, rtcOk);
  ui_set_battery(battV, battPct);
#if USE_BLE
  ui_set_ble(deviceConnected ? 1 : 0);
#endif
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // No interrupts: IMU and MAX30102 are polled in loop()
  g_i2cMux = xSemaphoreCreateMutex();

  // Use the ESP32-S3 AMOLED I2C pins
  I2Cbus.begin(SDA_PIN, SCL_PIN);
  I2Cbus.setClock(100000);   // keep 100 kHz: MLX90614 max is 100 kHz

  // Sensor + touch init run BEFORE the LVGL task exists, so no locking needed here
  initMAX();
  initQMI8658(); // Replaced MPU initialization with QMI8658
  initMLX90614();
  Serial.println(touch_init() ? "FT3168 touch OK" : "FT3168 touch NOT found (0x38)");

  // Display first: it needs the biggest contiguous internal-RAM blocks (LVGL draw buffers)
  lcd_lvgl_Init();   // AMOLED + LVGL + touch polling (own task on core 0)
  Serial.printf("heap after display: free %u, largest DMA block %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

  rtcSyncFromBuild();
  updateBattery();
  if (sdlog_begin()) {
    char hdr[SDLOG_LINE_MAX];
    snprintf(hdr, sizeof(hdr), "# SPANDAN log | start RTC=%s | build %s %s | period=%lu ms | VBAT=%.2f",
             rtcStr, __DATE__, __TIME__, (unsigned long)LOG_PERIOD_MS, battV);
    sdlog_write(hdr);
  } else {
    Serial.printf("SD not available (%s) - continuing without logging\n", sdlog_last_error());
  }
  Serial.printf("heap after SD: free %u, largest DMA block %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

#if USE_BLE
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(517);                       // the app asks for 512 on Android; allow it
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("BLE advertising as %s\n", BLE_DEVICE_NAME);
  Serial.printf("heap after BLE: free %u, largest block %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#endif

  Serial.println("\n" BLE_DEVICE_NAME " READY (ESP32-S3 AMOLED)  firmware: " FW_LABEL ", build " __DATE__ " " __TIME__);
  Serial.println("Commands: F = 30 s FIFO diagnostic (just press F, then Send), T2026-10-13 15:30:00 = set clock");
  Serial.println("Columns: FIFO_IR,FIFO_RED,S1_IR,S1_RED,IR_DC,IR_AC,RED_DC,RED_AC,HR,SpO2,ObjT,AmbT,accX,accY,accZ,gX,gY,gZ,ST");
}

void loop() {
  i2cLock();
  updateIMU();
  updateTempMLX();
  i2cUnlock();
  uiPush();
  logPush();
  infoPush();
  pollSerialCmd();
  diagService();
  bleNotify();
  {   // heartbeat every 5 s so the Serial Monitor shows the sketch is alive even without a finger on the sensor
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat >= 5000) {
      lastBeat = millis();
      Serial.printf("[alive %lus] ST=%s diag=%s sd=%s\n", (unsigned long)(millis() / 1000),
                    gyroMotion ? "IMU_MOTION" : statusMsg, diagActive ? "capturing" : "idle", sdlog_active() ? "ok" : "off");
    }
  }

  i2cLock();
  uint8_t dgWr = 0, dgOvf = 0, dgRd = 0;
#if MAX_GATE_NEW_SAMPLE
  diagReadPtrs(&dgWr, &dgOvf, &dgRd);                   // FIFO pointers: is a new sample waiting?
  if (((dgWr - dgRd) & 0x1F) == 0 && dgOvf == 0) {      // empty: nothing new, do not feed stale data to the algorithm
    i2cUnlock();
    if (diagActive) diagRecord(dgWr, dgOvf, dgRd, 0, 0);
    delay(2);
    return;
  }
#else
  if (diagActive) diagReadPtrs(&dgWr, &dgOvf, &dgRd);   // pointers first, then the sample read below
#endif
  I2Cbus.beginTransmission(MAX30102_ADDR);
  I2Cbus.write(0x07);
  I2Cbus.endTransmission(false);
  if (I2Cbus.requestFrom(MAX30102_ADDR, (uint8_t)6) != 6) { i2cUnlock(); return; }

  uint32_t red = ((uint32_t)I2Cbus.read() << 16) |
                 ((uint32_t)I2Cbus.read() << 8) |
                 I2Cbus.read();
  uint32_t ir  = ((uint32_t)I2Cbus.read() << 16) |
                 ((uint32_t)I2Cbus.read() << 8) |
                 I2Cbus.read();
  i2cUnlock();   // all MAX30102 bytes read; rest of loop() does no I2C
  red &= 0x3FFFF;
  ir &= 0x3FFFF;
  if (diagActive) diagRecord(dgWr, dgOvf, dgRd, ir, red);
  maxDataReady = false;

  uint32_t adcIR = ir;
  uint32_t adcRed = red;
  lastIR = adcIR; lastRed = adcRed;

  uint8_t irB0 = (ir >> 16) & 0x03;
  uint8_t irB1 = (ir >> 8) & 0xFF;
  uint8_t irB2 = ir & 0xFF;
  uint8_t redB0 = (red >> 16) & 0x03;
  uint8_t redB1 = (red >> 8) & 0xFF;
  uint8_t redB2 = red & 0xFF;
  char s1ir[12], s1red[12];
  snprintf(s1ir, sizeof(s1ir), "%02X %02X %02X", irB0, irB1, irB2);
  snprintf(s1red, sizeof(s1red), "%02X %02X %02X", redB0, redB1, redB2);

  sampleCounter++;

  if (dcInitialized) {
    prevIrDC = irDC;
    prevRedDC = redDC;
  }

  if (!dcInitialized) {
    irDC = (float)adcIR;
    redDC = (float)adcRed;
    dcInitialized = true;
  } else {
    irDC = DC_ALPHA * irDC + (1.0f - DC_ALPHA) * (float)adcIR;
    redDC = DC_ALPHA * redDC + (1.0f - DC_ALPHA) * (float)adcRed;
  }

  float irAC = (float)adcIR - irDC;
  float redAC = (float)adcRed - redDC;
  lastIrAC = irAC; lastRedAC = redAC;

  if (!hasContact(adcIR, adcRed)) {
    isSignalValid = false;
    uint32_t nowMs = millis();
    if (nowMs - lastStatusMs >= 500) {
      Serial.print("NO_CONTACT  IR=");
      Serial.print(adcIR);
      Serial.print("  RED=");
      Serial.println(adcRed);
      lastStatusMs = nowMs;
    }
    setStatus("NO_CONTACT");
    resetDerivedReadings();
    dcInitialized = false;
    hasPrevSample = false;
    sampleCounter = 0;
    motionStreak = 0;
    delay(50);
    return;
  }

  if (sampleCounter < WARMUP_SAMPLES) {
    isSignalValid = false;
    printStatusThrottled("WARMUP");
    prevAdcIR = adcIR;
    prevAdcRed = adcRed;
    hasPrevSample = true;
    return;
  }

  if (detectMotionArtifact(adcIR, adcRed)) {
    motionStreak++;
    isSignalValid = false;
    printStatusThrottled("MOTION");
    if (motionStreak >= MAX_BAD_MOTION_STREAK) {
      startPostMotionRecovery(adcIR, adcRed);
      sumIR_sq = sumRed_sq = sumIR = sumRed = 0;
      spo2Count = 0;
      rOutliers = 0;
      prevR = 0;
      spo2Valid = false;
    }
    prevAdcIR = adcIR;
    prevAdcRed = adcRed;
    hasPrevSample = true;
    return;
  }

  motionStreak = 0;
  isSignalValid = true;
  setStatus("OK");
  prevAdcIR = adcIR;
  prevAdcRed = adcRed;
  hasPrevSample = true;

  if (gyroMotion) {
    imuMotionStreak++;
    printStatusThrottled("IMU_MOTION");

    irPrevPrev = irPrev = 0;
    rising = false;

    if (imuMotionStreak >= SPO2_MOTION_FLUSH_AT) {
      sumIR_sq = sumRed_sq = sumIR = sumRed = 0;
      spo2Count = 0;
      rOutliers = 0;
      prevR = 0;
      spo2Valid = false;
      spo2StableCount = 0;
    }

    prevAdcIR = adcIR;
    prevAdcRed = adcRed;
    hasPrevSample = true;
    delay(2);
    return;
  }

  imuMotionStreak = 0;

  bool beatDetected = false;

  if (irAC > irPrev) {
    rising = true;
  } else if (rising && irAC < irPrev) {
    if (fabsf(irPrev) > MIN_SIGNAL_AMPLITUDE) {
      rising = false;
      beatDetected = true;
      uint32_t now = millis();

      if (lastBeatMs > 0) {
        uint32_t rr = now - lastBeatMs;

        if (rr < MIN_RR_MS) {
          rising = false;
        } else if (rr <= 1500) {
          beatIntervals[beatCount % 8] = rr;
          beatCount++;
          inconsistentBeats = 0;
          lastBeatMs = now;

          if (beatCount >= 4) {
            uint32_t tmpArr[8];
            int n = min(beatCount, 8);
            for (int i = 0; i < n; i++) tmpArr[i] = beatIntervals[i];
            uint32_t medianRR = getMedianInterval(tmpArr, n);
            float candidateHR = 60000.0f / (float)medianRR;

            if (hrSmoothed <= 0.0f) {
              hrSmoothed = candidateHR;
              hrStableCount = 1;
            } else {
              float rel = fabsf(candidateHR - hrSmoothed) / max(1.0f, hrSmoothed);
              if (rel <= HR_REL_TOLERANCE) hrStableCount++;
              else hrStableCount = 1;
              hrSmoothed = (1.0f - HR_EWMA_ALPHA) * hrSmoothed + HR_EWMA_ALPHA * candidateHR;
            }

            if (hrStableCount >= HR_STABLE_REQUIRED) {
              heartRate = hrSmoothed;
              hrValid = true;
              lastDisplayedHR = heartRate;
              lastGoodHRMs = millis();
            } else if (heartRate <= 0) {
              hrValid = false;
            }
          }
        } else {
          inconsistentBeats++;
          if (inconsistentBeats > 3) {
            beatCount = 0;
            hrValid = false;
            inconsistentBeats = 0;
          }
          lastBeatMs = now;
        }
      } else {
        lastBeatMs = now;
      }
    } else {
      rising = false;
    }
  }

  irPrevPrev = irPrev;
  irPrev = irAC;

  sumIR_sq += irAC * irAC;
  sumRed_sq += redAC * redAC;
  sumIR += irDC;
  sumRed += redDC;
  spo2Count++;

  if (spo2Count >= SPO2_MIN_WINDOW_SAMPLES && sumIR_sq > 0 && irDC > 0 && redDC > 0) {
    float rmsIR = sqrtf(sumIR_sq / spo2Count);
    float rmsRed = sqrtf(sumRed_sq / spo2Count);
    float avgIR = sumIR / spo2Count;
    float avgRed = sumRed / spo2Count;

    float irPerf = rmsIR / avgIR;
    float redPerf = rmsRed / avgRed;
    float R = redPerf / irPerf;

    bool windowGood =
      irPerf >= MIN_SPO2_AC_DC_RATIO && irPerf <= MAX_SPO2_AC_DC_RATIO &&
      redPerf >= MIN_SPO2_AC_DC_RATIO && redPerf <= MAX_SPO2_AC_DC_RATIO &&
      R >= WRIST_R_MIN && R <= WRIST_R_MAX;

    float rChange = fabsf(R - prevR);
    bool candidateGood = windowGood && (prevR <= 0 || rChange <= SPO2_MAX_R_CHANGE);

    if (!windowGood) {
      markSpo2Outlier();
    } else if (!candidateGood) {
      markSpo2Outlier();
    } else {
      float candidateSpO2 = SPO2_COEFF_A - SPO2_COEFF_B * R;
      candidateSpO2 = constrain(candidateSpO2, 80.0f, 100.0f);

      if (spO2Smoothed > 0.0f) {
        float maxStep = 2.0f;
        if (candidateSpO2 > spO2Smoothed + maxStep) candidateSpO2 = spO2Smoothed + maxStep;
        if (candidateSpO2 < spO2Smoothed - maxStep) candidateSpO2 = spO2Smoothed - maxStep;
      }

      if (spO2Smoothed <= 0.0f) {
        spO2Smoothed = candidateSpO2;
        spo2StableCount = 1;
      } else {
        float relChange = fabsf(candidateSpO2 - spO2Smoothed) / max(1.0f, spO2Smoothed);
        if (relChange <= SPO2_REL_TOLERANCE) spo2StableCount++;
        else spo2StableCount = 1;

        spO2Smoothed = (1.0f - SPO2_EWMA_ALPHA) * spO2Smoothed + SPO2_EWMA_ALPHA * candidateSpO2;
      }

      if (spo2StableCount >= SPO2_STABLE_REQUIRED) {
        spO2 = spO2Smoothed;
        spo2Valid = true;
        rOutliers = 0;
        prevR = R;
      } else if (spO2 <= 0) {
        spo2Valid = false;
      }
    }

    if (spo2Count >= SPO2_RESET_WINDOW_SAMPLES) {
      sumIR_sq = sumRed_sq = sumIR = sumRed = 0;
      spo2Count = 0;
      rOutliers = 0;
    }
  }

  if (lastBeatMs > 0 && (millis() - lastBeatMs) > HR_TIMEOUT_MS) {
    hrValid = false;
  }

  if (millis() - lastPrint > 1000) {
    qualityScore = 100;
    if (gyroMotion) qualityScore -= 35;
    if (motionStreak > 0) qualityScore -= 20;
    if (!hrValid) qualityScore -= 20;
    if (!spo2Valid) qualityScore -= 20;
    if (rOutliers > 0) qualityScore -= min(20, rOutliers * 5);
    if (qualityScore < 0) qualityScore = 0;

    bool hrDisplayValid = hrValid ||
      (lastDisplayedHR > 0 && (millis() - lastGoodHRMs) <= HR_HOLD_AFTER_IMU_MS);

    String data =
      "FIFO_IR=" + String(adcIR) +
      ",FIFO_RED=" + String(adcRed) +
      ",S1_IR=" + String(s1ir) +
      ",S1_RED=" + String(s1red) +
      ",IR_DC=" + String(irDC, 1) +
      ",IR_AC=" + String(irAC, 1) +
      ",RED_DC=" + String(redDC, 1) +
      ",RED_AC=" + String(redAC, 1) +
      ",HR=" + (hrDisplayValid
                ? String(hrValid ? heartRate : lastDisplayedHR, 1) + (beatDetected && hrValid ? "*" : "")
                : String("--")) +
      ",SpO2=" + (spo2Valid && spO2 > 0 ? String(spO2, 1) + "%" : String("--")) +
      ",ObjT=" + String(mlxObjectTemp, 1) +
      ",AmbT=" + String(mlxAmbientTemp, 1) +
      ",accX=" + String(accX, 3) +
      ",accY=" + String(accY, 3) +
      ",accZ=" + String(accZ, 3) +
      ",accMag=" + String(accelMagRaw, 3) +
      ",accMagF=" + String(accelMagFilt, 3) +
      ",gMagF=" + String(gyroMagFilt, 2) +
      ",gX=" + String(gyroX, 2) +
      ",gY=" + String(gyroY, 2) +
      ",gZ=" + String(gyroZ, 2) +
      ",ST=" + String(gyroMotion ? "IMU_MOTION" : statusMsg) +
      ",Q=" + String(qualityScore);

    Serial.println(data);


    lastPrint = millis();
  }

  delay(2);
}
