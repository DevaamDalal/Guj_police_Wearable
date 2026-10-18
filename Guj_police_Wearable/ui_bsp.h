#ifndef UI_BSP_H
#define UI_BSP_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

void lcd_lvgl_Init(void);   // display + LVGL task (lcd_bsp.c)
const char *lcd_controller_name(void);   // "SH8601" or "CO5300" (lcd_bsp.c)
unsigned char lcd_controller_id(void);
void ui_create(void);       // build the UI; called by lcd_bsp.c with the LVGL lock held
void ui_show_page(int idx); // 0 = watch face, 1 = vitals, 2 = system (LVGL lock must be held)

// All setters below are thread-safe and non-blocking: call from any task, the LVGL task picks the values up.
void ui_set_values(float hr, bool hrOk, float spo2, bool spo2Ok,
                   float objT, float ambT, const char *status);
void ui_set_debug(float accMag, float gyroMag, float irDC, float redDC);
void ui_set_info(const char *line1, const char *line2, const char *line3);   // system page text lines
void ui_set_clock(int year, int mon, int day, int hour, int min, int sec, bool valid);
void ui_set_ble(int state);                                                  // 0 = advertising / waiting, 1 = connected, -1 = BLE off
void ui_set_battery(float volts, int percent, bool charging);                // percent < 0 = unknown; charging shows a bolt next to the %

#ifdef __cplusplus
}
#endif
#endif
