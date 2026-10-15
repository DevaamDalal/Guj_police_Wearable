// Spandan watch UI: three swipeable tiles (watch face / vitals / system) on a round 466x466 AMOLED.
// Black background (AMOLED pixels off). Fonts are generated into ui_font_*.c, so the UI does not depend
// on which Montserrat sizes are enabled in lv_conf.h.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "ui_bsp.h"
#include "touch_bsp.h"

LV_FONT_DECLARE(ui_font_xl)   // 110 px: 0-9 : -
LV_FONT_DECLARE(ui_font_l)    //  64 px: 0-9 - . % degree C
LV_FONT_DECLARE(ui_font_m)    //  28 px: ASCII + degree
LV_FONT_DECLARE(ui_font_s)    //  20 px: ASCII + degree

#define SCREEN         466
#define COL_WHITE      lv_color_hex(0xFFFFFF)
#define COL_GREY       lv_color_hex(0x8A8F98)
#define COL_DIM        lv_color_hex(0x202225)
#define COL_RED        lv_color_hex(0xFF4D4D)
#define COL_CYAN       lv_color_hex(0x4DD2FF)
#define COL_AMBER      lv_color_hex(0xFFB020)
#define COL_GREEN      lv_color_hex(0x4CD964)

typedef struct {
  float hr, spo2, objT, ambT;
  float accMag, gyroMag, irDC, redDC;
  bool hrOk, spo2Ok;
  char st[24];
  char info[3][40];
  int year, mon, day, hour, min, sec;
  bool timeOk;
  int ble;
  float battV;
  int battPct;
} ui_data_t;

static ui_data_t g_data;
static SemaphoreHandle_t g_mux = NULL;

static lv_obj_t *tv, *dots[3];
static int s_page = 0;

// tile 0: watch face
static lv_obj_t *arc_sec, *lbl_time, *lbl_date, *bat_fill, *lbl_bat, *lbl_mini, *lbl_ble;
// tile 1: vitals
static lv_obj_t *arc_ring, *lbl_hr, *lbl_spo2, *lbl_temp, *lbl_status;
// tile 2: system
static lv_obj_t *lbl_acc, *lbl_gyr, *lbl_ir, *lbl_red, *lbl_touch, *lbl_i1, *lbl_i2, *lbl_i3;

static void set_text(lv_obj_t *o, const char *s) {
  if (strcmp(lv_label_get_text(o), s) != 0) lv_label_set_text(o, s);
}

static int day_of_week(int y, int m, int d) {          // 0 = Sunday
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, const char *txt, lv_coord_t y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, txt);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
  return l;
}

static lv_obj_t *make_ring(lv_obj_t *parent, lv_color_t bg, lv_color_t fg, int width) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_remove_style_all(a);                           // no knob, no theme styling
  lv_obj_set_size(a, SCREEN - 14, SCREEN - 14);
  lv_obj_center(a);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_arc_set_rotation(a, 270);
  lv_arc_set_bg_angles(a, 0, 360);
  lv_arc_set_range(a, 0, 60);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, bg, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, fg, LV_PART_INDICATOR);
  return a;
}

static void style_plain(lv_obj_t *o) {
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void page_changed_cb(lv_event_t *e) {
  (void)e;
  lv_obj_t *act = lv_tileview_get_tile_act(tv);
  if (act) s_page = (int)(intptr_t)lv_obj_get_user_data(act);   // NULL until the first layout pass
  for (int i = 0; i < 3; i++) {
    lv_obj_set_style_bg_color(dots[i], i == s_page ? COL_WHITE : lv_color_hex(0x40444A), 0);
    lv_obj_set_size(dots[i], i == s_page ? 10 : 8, i == s_page ? 10 : 8);
  }
}

void ui_show_page(int idx) {
  if (idx < 0 || idx > 2) return;
  lv_obj_set_tile_id(tv, (uint32_t)idx, 0, LV_ANIM_OFF);
  s_page = idx;
  page_changed_cb(NULL);
}

// ---------------------------------------------------------------- tile builders
static void build_face(lv_obj_t *t) {
  arc_sec  = make_ring(t, COL_DIM, COL_CYAN, 6);
  make_label(t, &ui_font_s, COL_GREY, "SPANDAN", -150);
  lbl_ble  = make_label(t, &ui_font_s, COL_GREY, "", -118);
  lbl_time = make_label(t, &ui_font_xl, COL_WHITE, "--:--", -30);
  lbl_date = make_label(t, &ui_font_m, COL_GREY, "RTC not set", 60);

  // battery icon built from plain rectangles
  lv_obj_t *row = lv_obj_create(t);
  style_plain(row);
  lv_obj_set_size(row, 150, 28);
  lv_obj_align(row, LV_ALIGN_CENTER, 0, 118);
  lv_obj_t *body = lv_obj_create(row);
  style_plain(body);
  lv_obj_set_size(body, 46, 24);
  lv_obj_set_pos(body, 0, 2);
  lv_obj_set_style_border_width(body, 2, 0);
  lv_obj_set_style_border_color(body, COL_GREY, 0);
  lv_obj_set_style_radius(body, 5, 0);
  lv_obj_t *nub = lv_obj_create(row);
  style_plain(nub);
  lv_obj_set_size(nub, 4, 10);
  lv_obj_set_pos(nub, 47, 9);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(nub, COL_GREY, 0);
  bat_fill = lv_obj_create(row);
  style_plain(bat_fill);
  lv_obj_set_size(bat_fill, 38, 16);
  lv_obj_set_pos(bat_fill, 4, 6);
  lv_obj_set_style_bg_opa(bat_fill, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bat_fill, COL_GREEN, 0);
  lv_obj_set_style_radius(bat_fill, 2, 0);
  lbl_bat = lv_label_create(row);
  lv_obj_set_style_text_font(lbl_bat, &ui_font_m, 0);
  lv_obj_set_style_text_color(lbl_bat, COL_WHITE, 0);
  lv_label_set_text(lbl_bat, "--%");
  lv_obj_set_pos(lbl_bat, 62, -1);

  lbl_mini = make_label(t, &ui_font_s, COL_RED, "HR -- bpm    SpO2 --%", 162);
}

static void build_vitals(lv_obj_t *t) {
  arc_ring = make_ring(t, COL_DIM, COL_GREY, 6);
  lv_arc_set_value(arc_ring, 60);
  make_label(t, &ui_font_s, COL_GREY, "HEART RATE", -180);
  lbl_hr = make_label(t, &ui_font_xl, COL_RED, "--", -92);
  make_label(t, &ui_font_s, COL_GREY, "bpm", -10);
  make_label(t, &ui_font_s, COL_GREY, "SpO2", 20);
  lbl_spo2 = make_label(t, &ui_font_l, COL_CYAN, "--", 72);
  lbl_temp = make_label(t, &ui_font_m, COL_AMBER, "--.- C", 130);
  lbl_status = make_label(t, &ui_font_s, COL_GREY, "STARTING", 168);
}

static void build_system(lv_obj_t *t) {
  make_label(t, &ui_font_s, COL_GREY, "SYSTEM", -165);
  lbl_acc   = make_label(t, &ui_font_s, COL_GREEN, "Acc --",      -120);
  lbl_gyr   = make_label(t, &ui_font_s, COL_CYAN,  "Gyro --",     -90);
  lbl_ir    = make_label(t, &ui_font_s, COL_AMBER, "IR DC --",    -60);
  lbl_red   = make_label(t, &ui_font_s, COL_RED,   "RED DC --",   -30);
  lbl_touch = make_label(t, &ui_font_s, COL_WHITE, "Touch: --",     0);
  lbl_i1    = make_label(t, &ui_font_s, COL_GREEN, "Batt --",      45);
  lbl_i2    = make_label(t, &ui_font_s, COL_CYAN,  "SD: --",       80);
  lbl_i3    = make_label(t, &ui_font_s, COL_AMBER, "RTC --",      115);
}

// ---------------------------------------------------------------- periodic refresh (LVGL task, lock held)
static void update_face(const ui_data_t *d) {
  static const char *wd[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *mn[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  char b[48];
  if (d->timeOk) {
    snprintf(b, sizeof(b), "%02d:%02d", d->hour % 24, d->min % 60);
    set_text(lbl_time, b);
    int m = (d->mon >= 1 && d->mon <= 12) ? d->mon : 1;
    snprintf(b, sizeof(b), "%s %d %s", wd[day_of_week(d->year, m, d->day)], d->day, mn[m - 1]);
    set_text(lbl_date, b);
    if (lv_arc_get_value(arc_sec) != d->sec) lv_arc_set_value(arc_sec, d->sec);
  } else {
    set_text(lbl_time, "--:--");
    set_text(lbl_date, "RTC not set");
    lv_arc_set_value(arc_sec, 0);
  }
  if (d->battPct >= 0) {
    snprintf(b, sizeof(b), "%d%%", d->battPct);
    set_text(lbl_bat, b);
    int w = (38 * d->battPct) / 100;
    lv_obj_set_width(bat_fill, w < 2 ? 2 : w);
    lv_obj_set_style_bg_color(bat_fill, d->battPct > 30 ? COL_GREEN : (d->battPct > 15 ? COL_AMBER : COL_RED), 0);
  } else {
    set_text(lbl_bat, "--%");
    lv_obj_set_width(bat_fill, 2);
    lv_obj_set_style_bg_color(bat_fill, COL_GREY, 0);
  }
  if (d->ble == 1)      { set_text(lbl_ble, "BT connected"); lv_obj_set_style_text_color(lbl_ble, COL_CYAN, 0); }
  else if (d->ble == 0) { set_text(lbl_ble, "BT waiting");   lv_obj_set_style_text_color(lbl_ble, COL_GREY, 0); }
  else                  { set_text(lbl_ble, ""); }
  char hr[16], sp[16];
  if (d->hrOk) snprintf(hr, sizeof(hr), "%d", (int)(d->hr + 0.5f)); else strcpy(hr, "--");
  if (d->spo2Ok) snprintf(sp, sizeof(sp), "%d", (int)(d->spo2 + 0.5f)); else strcpy(sp, "--");
  snprintf(b, sizeof(b), "HR %s bpm    SpO2 %s%%", hr, sp);
  set_text(lbl_mini, b);
}

static void update_vitals(const ui_data_t *d) {
  char b[48];
  if (d->hrOk) snprintf(b, sizeof(b), "%d", (int)(d->hr + 0.5f)); else snprintf(b, sizeof(b), "--");
  set_text(lbl_hr, b);
  if (d->spo2Ok) snprintf(b, sizeof(b), "%d%%", (int)(d->spo2 + 0.5f)); else snprintf(b, sizeof(b), "--");
  set_text(lbl_spo2, b);
  snprintf(b, sizeof(b), "%.1f\xC2\xB0""C", d->objT);
  set_text(lbl_temp, b);
  set_text(lbl_status, d->st);
  lv_color_t c = !strcmp(d->st, "OK") ? COL_GREEN
               : (!strcmp(d->st, "NO_CONTACT") || !strcmp(d->st, "STARTING")) ? COL_GREY : COL_AMBER;
  lv_obj_set_style_text_color(lbl_status, c, 0);
  lv_obj_set_style_arc_color(arc_ring, c, LV_PART_INDICATOR);
}

static void update_system(const ui_data_t *d) {
  char b[48];
  snprintf(b, sizeof(b), "Acc %.3f g", d->accMag);     set_text(lbl_acc, b);
  snprintf(b, sizeof(b), "Gyro %.1f dps", d->gyroMag);  set_text(lbl_gyr, b);
  snprintf(b, sizeof(b), "IR DC %d", (int)d->irDC);    set_text(lbl_ir, b);
  snprintf(b, sizeof(b), "RED DC %d", (int)d->redDC);  set_text(lbl_red, b);
  int tx, ty; bool down;
  touch_get_last(&tx, &ty, &down);
  if (tx < 0) snprintf(b, sizeof(b), "Touch: --");
  else        snprintf(b, sizeof(b), "Touch %d,%d %s", tx, ty, down ? "DOWN" : "up");
  set_text(lbl_touch, b);
  set_text(lbl_i1, d->info[0]);
  set_text(lbl_i2, d->info[1]);
  set_text(lbl_i3, d->info[2]);
}

static void ui_timer_cb(lv_timer_t *t) {
  (void)t;
  ui_data_t d;
  if (xSemaphoreTake(g_mux, 0) != pdTRUE) return;
  d = g_data;
  xSemaphoreGive(g_mux);
  if (s_page == 0) update_face(&d);       // only the visible tile is refreshed (saves CPU / redraws)
  else if (s_page == 1) update_vitals(&d);
  else update_system(&d);
}

// ---------------------------------------------------------------- creation
void ui_create(void) {
  g_mux = xSemaphoreCreateMutex();
  memset(&g_data, 0, sizeof(g_data));
  strcpy(g_data.st, "STARTING");
  g_data.battPct = -1;
  g_data.ble = -1;
  strcpy(g_data.info[0], "Batt --"); strcpy(g_data.info[1], "SD: --"); strcpy(g_data.info[2], "RTC --");

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  tv = lv_tileview_create(scr);
  lv_obj_set_size(tv, SCREEN, SCREEN);
  lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *t0 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
  lv_obj_t *t1 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
  lv_obj_t *t2 = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);
  lv_obj_t *tiles[3] = {t0, t1, t2};
  for (int i = 0; i < 3; i++) {
    lv_obj_set_user_data(tiles[i], (void *)(intptr_t)i);
    lv_obj_set_style_bg_opa(tiles[i], LV_OPA_TRANSP, 0);
  }
  build_face(t0);
  build_vitals(t1);
  build_system(t2);

  // page dots live on the top layer so they do not scroll with the tiles
  lv_obj_t *dotrow = lv_obj_create(lv_layer_top());
  style_plain(dotrow);
  lv_obj_set_size(dotrow, 60, 12);
  lv_obj_align(dotrow, LV_ALIGN_BOTTOM_MID, 0, -14);
  for (int i = 0; i < 3; i++) {
    dots[i] = lv_obj_create(dotrow);
    style_plain(dots[i]);
    lv_obj_set_size(dots[i], 8, 8);
    lv_obj_set_pos(dots[i], 4 + i * 20, 2);
    lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
  }
  lv_obj_add_event_cb(tv, page_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_set_tile_id(tv, 0, 0, LV_ANIM_OFF);
  s_page = 0;
  page_changed_cb(NULL);
  lv_timer_create(ui_timer_cb, 200, NULL);
}

// ---------------------------------------------------------------- setters (any task)
void ui_set_values(float hr, bool hrOk, float spo2, bool spo2Ok,
                   float objT, float ambT, const char *status) {
  if (!g_mux) return;
  if (xSemaphoreTake(g_mux, 0) != pdTRUE) return;   // never block the sensor loop
  g_data.hr = hr;     g_data.hrOk = hrOk;
  g_data.spo2 = spo2; g_data.spo2Ok = spo2Ok;
  g_data.objT = objT; g_data.ambT = ambT;
  strncpy(g_data.st, status, sizeof(g_data.st) - 1);
  g_data.st[sizeof(g_data.st) - 1] = '\0';
  xSemaphoreGive(g_mux);
}

void ui_set_debug(float accMag, float gyroMag, float irDC, float redDC) {
  if (!g_mux) return;
  if (xSemaphoreTake(g_mux, 0) != pdTRUE) return;
  g_data.accMag = accMag; g_data.gyroMag = gyroMag;
  g_data.irDC = irDC;     g_data.redDC = redDC;
  xSemaphoreGive(g_mux);
}

void ui_set_info(const char *line1, const char *line2, const char *line3) {
  if (!g_mux) return;
  if (xSemaphoreTake(g_mux, 0) != pdTRUE) return;
  strncpy(g_data.info[0], line1, sizeof(g_data.info[0]) - 1);
  strncpy(g_data.info[1], line2, sizeof(g_data.info[1]) - 1);
  strncpy(g_data.info[2], line3, sizeof(g_data.info[2]) - 1);
  for (int i = 0; i < 3; i++) g_data.info[i][sizeof(g_data.info[i]) - 1] = '\0';
  xSemaphoreGive(g_mux);
}

void ui_set_clock(int year, int mon, int day, int hour, int min, int sec, bool valid) {
  if (!g_mux) return;
  if (xSemaphoreTake(g_mux, 0) != pdTRUE) return;
  g_data.year = year; g_data.mon = mon; g_data.day = day;
  g_data.hour = hour; g_data.min = min; g_data.sec = sec;
  g_data.timeOk = valid;
  xSemaphoreGive(g_mux);
}

void ui_set_battery(float volts, int percent) {
  if (!g_mux) return;
  if (xSemaphoreTake(g_mux, 0) != pdTRUE) return;
  g_data.battV = volts;
  g_data.battPct = percent;
  xSemaphoreGive(g_mux);
}

void ui_set_ble(int state) {
  if (!g_mux) return;
  if (xSemaphoreTake(g_mux, 0) != pdTRUE) return;
  g_data.ble = state;
  xSemaphoreGive(g_mux);
}
