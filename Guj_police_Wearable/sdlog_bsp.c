// SD card logging over SPI (SPI3_HOST; the display uses SPI2). Pins from the Waveshare demo:
// CS=38 MOSI=39 MISO=40 CLK=41. A low-priority task does all SD writes so a slow card can
// never stall the sensor loop. The card is never auto-formatted.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdlog_bsp.h"

#define SD_PIN_MISO   40
#define SD_PIN_MOSI   39
#define SD_PIN_CLK    41
#define SD_PIN_CS     38
#define SD_HOST_ID    SPI3_HOST
#define SD_MOUNT      "/sd_card"
#define SDLOG_QUEUE_LEN 40
#define SDLOG_FLUSH_MS  2000

static sdmmc_card_t *s_card = NULL;
static FILE *s_file = NULL;
static QueueHandle_t s_q = NULL;
static volatile int s_state = 0;            // 0 = no card, 1 = logging, 2 = write error
static volatile uint32_t s_rows = 0, s_dropped = 0;
static char s_name[12] = "";
static char s_err[64] = "";
const char *sdlog_last_error(void) { return s_err; }

static void sdlog_task(void *arg)
{
  static char line[SDLOG_LINE_MAX];
  TickType_t lastFlush = xTaskGetTickCount();
  for (;;)
  {
    if (xQueueReceive(s_q, line, pdMS_TO_TICKS(500)) == pdTRUE && s_state == 1)
    {
      if (fputs(line, s_file) < 0 || fputc('\n', s_file) < 0) s_state = 2;
      else s_rows++;
    }
    if (s_state == 1 && (xTaskGetTickCount() - lastFlush) >= pdMS_TO_TICKS(SDLOG_FLUSH_MS))
    {
      if (fflush(s_file) != 0) s_state = 2;
      else fsync(fileno(s_file));
      lastFlush = xTaskGetTickCount();
    }
  }
}

bool sdlog_begin(void)
{
  esp_vfs_fat_sdmmc_mount_config_t mount_config =
  {
    .format_if_mount_failed = false,        // never wipe a card
    .max_files = 3,
    .allocation_unit_size = 16 * 1024,
  };
  spi_bus_config_t bus_cfg =
  {
    .mosi_io_num = SD_PIN_MOSI,
    .miso_io_num = SD_PIN_MISO,
    .sclk_io_num = SD_PIN_CLK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4000,
  };
  esp_err_t e = spi_bus_initialize(SD_HOST_ID, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { snprintf(s_err, sizeof(s_err), "spi bus init failed: %s", esp_err_to_name(e)); return false; }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = SD_PIN_CS;
  slot_config.host_id = SD_HOST_ID;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SD_HOST_ID;
  host.max_freq_khz = 10000;                // 10 MHz: plenty for ~5 KB/s, friendlier to old cards

  e = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot_config, &mount_config, &s_card);
  if (e != ESP_OK || s_card == NULL)
  {
    snprintf(s_err, sizeof(s_err), "mount failed: %s (card seated? FAT32?)", esp_err_to_name(e));
    return false;
  }
  sdmmc_card_print_info(stdout, s_card);

  char path[40];
  struct stat st;
  int n;
  for (n = 1; n < 10000; n++)
  {
    snprintf(s_name, sizeof(s_name), "LOG%04d", n);
    snprintf(path, sizeof(path), SD_MOUNT "/%s.TXT", s_name);
    if (stat(path, &st) != 0) break;        // first unused number
  }
  s_file = fopen(path, "w");
  if (s_file == NULL) { snprintf(s_err, sizeof(s_err), "cannot create %s", path); return false; }

  s_q = xQueueCreate(SDLOG_QUEUE_LEN, SDLOG_LINE_MAX);
  if (s_q == NULL) { fclose(s_file); s_file = NULL; snprintf(s_err, sizeof(s_err), "log queue allocation failed (RAM)"); return false; }
  s_state = 1;
  xTaskCreatePinnedToCore(sdlog_task, "sdlog", 6 * 1024, NULL, 1, NULL, 0);
  printf("SD: logging to %s\n", path);
  return true;
}

bool sdlog_active(void) { return s_state == 1; }

void sdlog_write(const char *line)
{
  if (s_state != 1 || s_q == NULL) return;
  char tmp[SDLOG_LINE_MAX];
  strncpy(tmp, line, SDLOG_LINE_MAX - 1);
  tmp[SDLOG_LINE_MAX - 1] = '\0';
  if (xQueueSend(s_q, tmp, 0) != pdTRUE) s_dropped++;
}

void sdlog_status(char *out, size_t n)
{
  if (s_state == 0)      snprintf(out, n, "SD: none");
  else if (s_state == 2) snprintf(out, n, "SD %s: WRITE ERROR", s_name);
  else if (s_dropped)    snprintf(out, n, "SD %s  %lu rows (drop %lu)", s_name, (unsigned long)s_rows, (unsigned long)s_dropped);
  else                   snprintf(out, n, "SD %s  %lu rows", s_name, (unsigned long)s_rows);
}

static FILE *s_aux = NULL;

bool sdlog_aux_open(const char *prefix, const char *ext, char *nameOut, size_t n)
{
  if (s_state != 1 || s_aux != NULL) return false;      // needs a mounted, healthy card
  char path[48];
  struct stat st;
  int i;
  for (i = 1; i < 10000; i++)
  {
    snprintf(nameOut, n, "%s%04d.%s", prefix, i, ext);
    snprintf(path, sizeof(path), SD_MOUNT "/%s", nameOut);
    if (stat(path, &st) != 0) break;
  }
  s_aux = fopen(path, "w");
  return s_aux != NULL;
}

bool sdlog_aux_write(const char *text)
{
  if (s_aux == NULL) return false;
  return fputs(text, s_aux) >= 0;
}

void sdlog_aux_close(void)
{
  if (s_aux != NULL) { fflush(s_aux); fsync(fileno(s_aux)); fclose(s_aux); s_aux = NULL; }
}
