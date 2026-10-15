// PCF85063 RTC (I2C 0x51) on the shared I2Cbus, serialised with g_i2cMux.
// Do NOT call these while already holding g_i2cMux (the mutex is not recursive).
#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rtc_bsp.h"

extern TwoWire I2Cbus;
extern SemaphoreHandle_t g_i2cMux;

#define PCF85063_ADDR 0x51
#define PCF85063_REG_SECONDS 0x04   // 0x04..0x0A = sec,min,hour,day,weekday,month,year (BCD)

static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

extern "C" bool rtc_get(RtcTime *t) {
  uint8_t d[7];
  if (xSemaphoreTake(g_i2cMux, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  bool ok = false;
  I2Cbus.beginTransmission(PCF85063_ADDR);
  I2Cbus.write(PCF85063_REG_SECONDS);
  if (I2Cbus.endTransmission(false) == 0 &&
      I2Cbus.requestFrom((uint8_t)PCF85063_ADDR, (uint8_t)7) == 7) {
    for (int i = 0; i < 7; i++) d[i] = I2Cbus.read();
    ok = true;
  }
  xSemaphoreGive(g_i2cMux);
  if (!ok) return false;
  if (d[0] & 0x80) return false;            // OS/VL flag: clock integrity not guaranteed
  t->sec   = bcd2dec(d[0] & 0x7F);
  t->min   = bcd2dec(d[1] & 0x7F);
  t->hour  = bcd2dec(d[2] & 0x3F);
  t->day   = bcd2dec(d[3] & 0x3F);
  t->month = bcd2dec(d[5] & 0x1F);
  t->year  = 2000 + bcd2dec(d[6]);
  return true;
}

extern "C" bool rtc_set(const RtcTime *t) {
  // weekday (0=Sunday) via Zeller-like formula, same as Waveshare demo
  int y = t->year, m = t->month, dd = t->day;
  if (m == 1 || m == 2) { m += 12; y--; }
  int week = ((dd + 2 * m + 3 * (m + 1) / 5 + y + y / 4 - y / 100 + y / 400) % 7 + 1) % 7;

  if (xSemaphoreTake(g_i2cMux, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  I2Cbus.beginTransmission(PCF85063_ADDR);
  I2Cbus.write(PCF85063_REG_SECONDS);
  I2Cbus.write(dec2bcd(t->sec));          // bit 7 = 0 clears the VL flag
  I2Cbus.write(dec2bcd(t->min));
  I2Cbus.write(dec2bcd(t->hour));
  I2Cbus.write(dec2bcd(t->day));
  I2Cbus.write(dec2bcd(week));
  I2Cbus.write(dec2bcd(t->month));
  I2Cbus.write(dec2bcd(t->year - 2000));
  uint8_t err = I2Cbus.endTransmission();
  xSemaphoreGive(g_i2cMux);
  return err == 0;
}
