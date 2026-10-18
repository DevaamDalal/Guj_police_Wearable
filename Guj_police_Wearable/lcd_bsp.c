#include "lcd_bsp.h"
#include "esp_lcd_sh8601.h"
#include "lcd_config.h"
#include "ui_bsp.h"
#include "touch_bsp.h"
#include "read_lcd_id_bsp.h"
static SemaphoreHandle_t lvgl_mux = NULL; //mutex semaphores
#define LCD_HOST    SPI2_HOST

#define EXAMPLE_Rotate_90
#define SH8601_ID 0x86
#define CO5300_ID 0xff
static uint8_t READ_LCD_ID = 0x00; 

static const sh8601_lcd_init_cmd_t sh8601_lcd_init_cmds[] = 
{
  {0x11, (uint8_t []){0x00}, 0, 120},
  {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
  {0x35, (uint8_t []){0x00}, 1, 0},
  {0x53, (uint8_t []){0x20}, 1, 10},
  {0x51, (uint8_t []){0x00}, 1, 10},
  {0x29, (uint8_t []){0x00}, 0, 10},
  {0x51, (uint8_t []){0xFF}, 1, 0},
  //{0x36, (uint8_t []){0x80}, 1, 0},
};
static const sh8601_lcd_init_cmd_t co5300_lcd_init_cmds[] = 
{
  {0x11, (uint8_t []){0x00}, 0, 80},   
  {0xC4, (uint8_t []){0x80}, 1, 0},
  //{0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
  //{0x35, (uint8_t []){0x00}, 1, 0},//TE ON
  {0x53, (uint8_t []){0x20}, 1, 1},
  {0x63, (uint8_t []){0xFF}, 1, 1},
  {0x51, (uint8_t []){0x00}, 1, 1},
  {0x29, (uint8_t []){0x00}, 0, 10},
  {0x51, (uint8_t []){0xFF}, 1, 0},
  //{0x36, (uint8_t []){0x60}, 1, 0},
};


// ---------------------------------------------------------------- screen power / brightness
// Every panel command is sent from the LVGL task (lv_timer below), so it never interleaves with a pixel transfer.
#define LCD_OPCODE_WRITE_CMD_  0x02
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static volatile int s_bright_pct = 100;
static volatile int s_timeout_s = 60;
static volatile bool s_screen_on = true;
static volatile bool s_req_wake = false, s_bright_dirty = true;
static volatile bool s_swallow_touch = false;
static volatile unsigned int s_settings_ver = 0;

void lcd_set_brightness(int pct) { if (pct < 10) pct = 10; if (pct > 100) pct = 100; if (pct != s_bright_pct) { s_bright_pct = pct; s_bright_dirty = true; s_settings_ver++; } }
int  lcd_get_brightness(void) { return s_bright_pct; }
void lcd_set_screen_timeout(int sec) { if (sec < 0) sec = 0; if (sec > 3600) sec = 3600; if (sec != s_timeout_s) { s_timeout_s = sec; s_settings_ver++; } }
int  lcd_get_screen_timeout(void) { return s_timeout_s; }
void lcd_screen_wake(void) { s_req_wake = true; }
bool lcd_screen_is_on(void) { return s_screen_on; }
unsigned int lcd_settings_version(void) { return s_settings_ver; }

static void panel_apply_brightness(void)
{
  if (s_io == NULL) return;
  uint8_t v = (uint8_t)((s_bright_pct * 255 + 50) / 100);
  esp_lcd_panel_io_tx_param(s_io, (LCD_OPCODE_WRITE_CMD_ << 24) | (0x51 << 8), &v, 1);   // 0x51 = write display brightness
}

static void screen_set(bool on)
{
  if (on == s_screen_on || s_panel == NULL) return;
  if (on)
  {
    s_screen_on = true;
    lv_obj_invalidate(lv_scr_act());
    lv_obj_invalidate(lv_layer_top());
    esp_lcd_panel_disp_on_off(s_panel, true);
    panel_apply_brightness();
    lv_disp_trig_activity(NULL);                  // restart the idle timer
  }
  else
  {
    esp_lcd_panel_disp_on_off(s_panel, false);    // AMOLED: no light, almost no power; frame memory is kept
    s_screen_on = false;
  }
  s_settings_ver++;
}

static void screen_timer_cb(lv_timer_t *t)        // LVGL task, every 100 ms
{
  (void)t;
  if (s_req_wake) { s_req_wake = false; screen_set(true); }
  if (s_bright_dirty) { s_bright_dirty = false; if (s_screen_on) panel_apply_brightness(); }
  if (s_screen_on && s_timeout_s > 0 && lv_disp_get_inactive_time(NULL) >= (uint32_t)s_timeout_s * 1000UL) screen_set(false);
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  uint16_t tp_x, tp_y;
  bool touched = touch_read(&tp_x, &tp_y);
  if (!s_screen_on)                               // screen off: a touch only wakes it, LVGL sees nothing
  {
    if (touched) { s_req_wake = true; s_swallow_touch = true; }
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (s_swallow_touch)                            // ignore the rest of the touch that woke the screen
  {
    if (touched) { data->state = LV_INDEV_STATE_RELEASED; return; }
    s_swallow_touch = false;
  }
  if (touched)
  {
    data->point.x = tp_x;   // raw panel coords: LVGL applies the display rotation itself
    data->point.y = tp_y;
    data->state = LV_INDEV_STATE_PRESSED;
  }
  else
  {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void lcd_lvgl_Init(void)
{
  READ_LCD_ID = read_lcd_id();
  static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
  static lv_disp_drv_t disp_drv;      // contains callback functions

  const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_PCLK,
                                                               EXAMPLE_PIN_NUM_LCD_DATA0,
                                                               EXAMPLE_PIN_NUM_LCD_DATA1,
                                                               EXAMPLE_PIN_NUM_LCD_DATA2,
                                                               EXAMPLE_PIN_NUM_LCD_DATA3,
                                                               EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * LCD_BIT_PER_PIXEL / 8);
  ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
  esp_lcd_panel_io_handle_t io_handle = NULL;

  const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_CS,
                                                                              example_notify_lvgl_flush_ready,
                                                                              &disp_drv);

  sh8601_vendor_config_t vendor_config = 
  {
    .flags = 
    {
      .use_qspi_interface = 1,
    },
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
  esp_lcd_panel_handle_t panel_handle = NULL;
  const esp_lcd_panel_dev_config_t panel_config = 
  {
    .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = LCD_BIT_PER_PIXEL,
    .vendor_config = &vendor_config,
  };
  vendor_config.init_cmds = (READ_LCD_ID == SH8601_ID) ? sh8601_lcd_init_cmds : co5300_lcd_init_cmds;
  vendor_config.init_cmds_size = (READ_LCD_ID == SH8601_ID) ? sizeof(sh8601_lcd_init_cmds) / sizeof(sh8601_lcd_init_cmds[0]) : sizeof(co5300_lcd_init_cmds) / sizeof(co5300_lcd_init_cmds[0]);
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(panel_handle, true));
  s_io = io_handle;
  s_panel = panel_handle;

  lv_init();
  lv_color_t *buf1 = heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(buf1);
  lv_color_t *buf2 = heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(buf2);
  lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = example_lvgl_flush_cb;
  disp_drv.rounder_cb = example_lvgl_rounder_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.user_data = panel_handle;
#ifdef EXAMPLE_Rotate_90
  disp_drv.sw_rotate = 1;
  disp_drv.rotated = LV_DISP_ROT_270;
#endif
  lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.disp = disp;
  indev_drv.read_cb = example_lvgl_touch_cb;
  // Touch feel: swipe starts after 3 px of movement (LVGL default 10) and glides further after a short flick
  // (scroll_throw default 10 = fastest stop); touch is polled every 10 ms (default 30).
  indev_drv.scroll_limit = TOUCH_SCROLL_LIMIT;
  indev_drv.scroll_throw = TOUCH_SCROLL_THROW;
  lv_indev_t *indev = lv_indev_drv_register(&indev_drv);
  if (indev && indev->driver && indev->driver->read_timer) lv_timer_set_period(indev->driver->read_timer, 10);
  lv_timer_create(screen_timer_cb, 100, NULL);

  const esp_timer_create_args_t lvgl_tick_timer_args = 
  {
    .callback = &example_increase_lvgl_tick,
    .name = "lvgl_tick"
  };
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  lvgl_mux = xSemaphoreCreateMutex(); //mutex semaphores
  assert(lvgl_mux);
  // Pinned to core 0 so LVGL never preempts the sensor loop() on core 1
  xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", 8 * 1024, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL, 0);
  if (example_lvgl_lock(-1)) 
  {   
    ui_create();

    // Release the mutex
    example_lvgl_unlock();
  }
}

static bool example_lvgl_lock(int timeout_ms)
{
  assert(lvgl_mux && "bsp_display_start must be called first");

  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

static void example_lvgl_unlock(void)
{
  assert(lvgl_mux && "bsp_display_start must be called first");
  xSemaphoreGive(lvgl_mux);
}
static void example_lvgl_port_task(void *arg)
{
  uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
  for(;;)
  {
    if (example_lvgl_lock(-1))
    {
      task_delay_ms = lv_timer_handler();
      
      example_lvgl_unlock();
    }
    if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    }
    else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}
static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
  lv_disp_flush_ready(disp_driver);
  return false;
}
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
  if (!s_screen_on) { lv_disp_flush_ready(drv); return; }        // screen off: skip the transfer (saves power and SPI time)
  const int offsetx1 = (READ_LCD_ID == SH8601_ID) ? area->x1 : area->x1 + 0x06;
  const int offsetx2 = (READ_LCD_ID == SH8601_ID) ? area->x2 : area->x2 + 0x06;
  const int offsety1 = area->y1;
  const int offsety2 = area->y2;

  esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}
void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
  uint16_t x1 = area->x1;
  uint16_t x2 = area->x2;

  uint16_t y1 = area->y1;
  uint16_t y2 = area->y2;

  // round the start of coordinate down to the nearest 2M number
  area->x1 = (x1 >> 1) << 1;
  area->y1 = (y1 >> 1) << 1;
  // round the end of coordinate up to the nearest 2N+1 number
  area->x2 = ((x2 >> 1) << 1) + 1;
  area->y2 = ((y2 >> 1) << 1) + 1;
}

const char *lcd_controller_name(void) { return (READ_LCD_ID == SH8601_ID) ? "SH8601" : "CO5300"; }
unsigned char lcd_controller_id(void) { return READ_LCD_ID; }
