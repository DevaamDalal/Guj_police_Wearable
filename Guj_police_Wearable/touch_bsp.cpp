// FT3168 / FT6146 touch over the SAME Wire bus object as the sensors (I2Cbus in the .ino),
// serialised with g_i2cMux. Does not use the legacy driver/i2c.h used in Waveshare's demo.
#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "touch_bsp.h"
#include "lcd_config.h"

extern TwoWire I2Cbus;
extern SemaphoreHandle_t g_i2cMux;

static volatile int s_x = -1, s_y = -1;
static volatile bool s_down = false;

extern "C" bool touch_init(void) {
  if (xSemaphoreTake(g_i2cMux, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  I2Cbus.beginTransmission(I2C_ADDR_FT3168);
  I2Cbus.write(0x00);   // device mode register
  I2Cbus.write(0x00);   // normal operating mode
  uint8_t err = I2Cbus.endTransmission();
  xSemaphoreGive(g_i2cMux);
  return err == 0;
}

extern "C" bool touch_read(uint16_t *x, uint16_t *y) {
  uint8_t b[5];
  if (xSemaphoreTake(g_i2cMux, pdMS_TO_TICKS(20)) != pdTRUE) return s_down;  // keep last state
  bool ok = false;
  I2Cbus.beginTransmission(I2C_ADDR_FT3168);
  I2Cbus.write(0x02);   // 0x02 = touch count, 0x03..0x06 = X/Y of point 1
  if (I2Cbus.endTransmission(false) == 0 &&
      I2Cbus.requestFrom((uint8_t)I2C_ADDR_FT3168, (uint8_t)5) == 5) {
    for (int i = 0; i < 5; i++) b[i] = I2Cbus.read();
    ok = true;
  }
  xSemaphoreGive(g_i2cMux);

  if (!ok || (b[0] & 0x0F) == 0) { s_down = false; return false; }

  uint16_t px = ((uint16_t)(b[1] & 0x0F) << 8) | b[2];
  uint16_t py = ((uint16_t)(b[3] & 0x0F) << 8) | b[4];
  if (px >= EXAMPLE_LCD_H_RES) px = EXAMPLE_LCD_H_RES - 1;
  if (py >= EXAMPLE_LCD_V_RES) py = EXAMPLE_LCD_V_RES - 1;
#if TOUCH_SERIAL_DEBUG
  if (!s_down) Serial.printf("TOUCH %u %u\n", px, py);
#endif
  s_x = px; s_y = py; s_down = true;
  *x = px; *y = py;
  return true;
}

extern "C" void touch_get_last(int *x, int *y, bool *down) {
  *x = s_x; *y = s_y; *down = s_down;
}
