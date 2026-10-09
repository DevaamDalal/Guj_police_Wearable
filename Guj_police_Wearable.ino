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
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

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

// ─────────────────────── BLE UUIDs ──────────────────────────
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLECharacteristic *pCharacteristic;
bool deviceConnected = false;

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) { deviceConnected = true; }
  void onDisconnect(BLEServer *pServer) {
    deviceConnected = false;
    BLEDevice::startAdvertising();
  }
};

TwoWire I2Cbus = TwoWire(0);

// ─────────────────────── INTERRUPT FLAGS ────────────────────
volatile bool imuDataReady = false;
volatile bool maxDataReady = false;
void IRAM_ATTR onIMUInterrupt() { imuDataReady = true; }
void IRAM_ATTR onMAXInterrupt() { maxDataReady = true; }

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

const int      SPO2_MIN_WINDOW_SAMPLES     = 18;
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

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(QMI_INT_PIN, INPUT);
  pinMode(MAX_INT_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(QMI_INT_PIN), onIMUInterrupt, RISING);
  attachInterrupt(digitalPinToInterrupt(MAX_INT_PIN), onMAXInterrupt, FALLING);

  // Use the ESP32-S3 AMOLED I2C pins
  I2Cbus.begin(SDA_PIN, SCL_PIN);
  I2Cbus.setClock(100000);

  initMAX();
  initQMI8658(); // Replaced MPU initialization with QMI8658
  initMLX90614();

  BLEDevice::init("SVNIT_CRL_16");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  BLEDevice::startAdvertising();

  Serial.println("\nSVNIT_CRL_16 READY (ESP32-S3 AMOLED)");
  Serial.println("Columns: FIFO_IR,FIFO_RED,S1_IR,S1_RED,IR_DC,IR_AC,RED_DC,RED_AC,HR,SpO2,ObjT,AmbT,accX,accY,accZ,gX,gY,gZ,ST");
}

void loop() {
  updateIMU();
  updateTempMLX();

  I2Cbus.beginTransmission(MAX30102_ADDR);
  I2Cbus.write(0x07);
  I2Cbus.endTransmission(false);
  if (I2Cbus.requestFrom(MAX30102_ADDR, (uint8_t)6) != 6) return;

  uint32_t red = ((uint32_t)I2Cbus.read() << 16) |
                 ((uint32_t)I2Cbus.read() << 8) |
                 I2Cbus.read();
  uint32_t ir  = ((uint32_t)I2Cbus.read() << 16) |
                 ((uint32_t)I2Cbus.read() << 8) |
                 I2Cbus.read();
  red &= 0x3FFFF;
  ir &= 0x3FFFF;
  maxDataReady = false;

  uint32_t adcIR = ir;
  uint32_t adcRed = red;

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

    if (deviceConnected) {
      pCharacteristic->setValue(data.c_str());
      pCharacteristic->notify();
    }

    lastPrint = millis();
  }

  delay(2);
}
