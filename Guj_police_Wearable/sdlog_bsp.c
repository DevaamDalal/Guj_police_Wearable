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
#include "esp_heap_caps.h"
#include "sdlog_bsp.h"

#define SD_PIN_MISO   40
#define SD_PIN_MOSI   39
#define SD_PIN_CLK    41
#define SD_PIN_CS     38
#define SD_HOST_ID    SPI3_HOST
#define SD_MOUNT      "/sd_card"
#define SDLOG_QUEUE_LEN 40
#define SDLOG_FLUSH_MS  2000
#define BUF_PATH        SD_MOUNT "/BLEBUF.TXT"
#define BUF_MAX_BYTES   (20UL * 1024UL * 1024UL)   // about 2 days of 1 Hz packets; beyond that new lines are dropped
#define BUF_LINE_MAX    260
// Queue items starting with these bytes are commands for the writer task, not log lines.
#define CMD_BUF_APPEND  '\x01'
#define CMD_RP_START    '\x02'
#define CMD_RP_STOP     '\x03'
#define CMD_EVENT       '\x04'
#define EVENT_PATH      SD_MOUNT "/SYSLOG.TXT"
#define EVENT_MAX_BYTES (8UL * 1024UL * 1024UL)   // stop adding events after 8 MB

static sdmmc_card_t *s_card = NULL;
static FILE *s_file = NULL;
static QueueHandle_t s_q = NULL;
static volatile int s_state = 0;            // 0 = no card, 1 = logging, 2 = write error
static volatile uint32_t s_rows = 0, s_dropped = 0;
static char s_name[12] = "";
static char s_err[64] = "";
const char *sdlog_last_error(void) { return s_err; }

// ---- offline buffer state (only touched by the writer task, except where marked) ----
static FILE *s_bf = NULL;                          // BLEBUF.TXT, opened "a+" (one handle: append at the end, read at s_rd_pos)
static long s_rd_pos = 0;                          // start of the next line to deliver
static volatile bool s_buf_pending = false;        // read by the loop task
static volatile unsigned long s_buf_bytes = 0, s_buf_dropped = 0, s_buf_skipped = 0;
static bool s_buf_dirty = false;
static volatile bool s_rp_on = false;              // replay requested
static volatile int s_rp_state = 0;                // 0 idle, 1 loop wants a line, 2 line ready, 3 finished
static char s_rp_line[BUF_LINE_MAX];
static volatile long s_rp_next = 0;

static FILE *s_ef = NULL;                          // SYSLOG.TXT, append mode
static unsigned long s_ev_bytes = 0, s_ev_dropped = 0;
static bool s_ev_dirty = false;

static void eventAppend(const char *text)
{
  if (s_ef == NULL) return;
  if (s_ev_bytes >= EVENT_MAX_BYTES) { s_ev_dropped++; return; }
  if (fputs(text, s_ef) < 0 || fputc('\n', s_ef) < 0) { s_ev_dropped++; return; }
  s_ev_bytes += strlen(text) + 1;
  s_ev_dirty = true;
}

static void bufAppend(const char *text)
{
  if (s_bf == NULL) s_bf = fopen(BUF_PATH, "a+");
  if (s_bf == NULL) { s_buf_dropped++; return; }
  if (s_buf_bytes >= BUF_MAX_BYTES) { s_buf_dropped++; return; }
  size_t n = strlen(text);
  fseek(s_bf, 0, SEEK_END);
  if (fputs(text, s_bf) < 0 || fputc('\n', s_bf) < 0) { s_buf_dropped++; return; }
  s_buf_bytes += n + 1;
  s_buf_pending = true;
  s_buf_dirty = true;
}

static void bufFinish(void)                        // everything delivered: remove the file
{
  if (s_bf != NULL) { fclose(s_bf); s_bf = NULL; }
  remove(BUF_PATH);
  s_rd_pos = 0; s_buf_bytes = 0; s_buf_dirty = false;
  s_buf_pending = false;
  s_rp_on = false;
  s_rp_state = 3;
}

static void bufReplayStart(void)
{
  s_rp_on = true;
  if (!s_buf_pending) { s_rp_state = 3; s_rp_on = false; return; }
  if (s_bf == NULL) s_bf = fopen(BUF_PATH, "a+");
  if (s_bf == NULL) { s_buf_pending = false; s_rp_state = 3; s_rp_on = false; return; }
  s_rp_state = 0;                                  // the loop task asks for the first line
}

static void bufReplayStop(void)
{
  s_rp_on = false;
  s_rp_state = 0;                                  // s_rd_pos only moves when a line is taken, so an unsent line is not lost
}

static void bufReplayRead(void)                    // called when s_rp_state == 1
{
  if (s_bf == NULL) { bufFinish(); return; }
  for (int tries = 0; tries < 8; tries++)
  {
    fseek(s_bf, s_rd_pos, SEEK_SET);
    if (fgets(s_rp_line, sizeof(s_rp_line), s_bf) == NULL)
    {
      bufFinish();                                 // end of file: all delivered
      return;
    }
    long next = ftell(s_bf);
    size_t n = strlen(s_rp_line);
    while (n > 0 && (s_rp_line[n - 1] == '\n' || s_rp_line[n - 1] == '\r')) s_rp_line[--n] = '\0';
    // A stored packet looks like  HR=...,Batt=...,...,BUF=1 . After a power cut the end of the file can hold torn or stale
    // data from an older file; such lines must never be sent to the app.
    if (n < 40 || strncmp(s_rp_line, "HR=", 3) != 0 || strstr(s_rp_line, ",Batt=") == NULL || strcmp(s_rp_line + n - 6, ",BUF=1") != 0)
    {
      s_buf_skipped++;
      s_rd_pos = next;
      continue;
    }
    s_rp_next = next;
    __sync_synchronize();
    s_rp_state = 2;
    return;
  }
}

static void sdlog_task(void *arg)
{
  static char line[SDLOG_LINE_MAX];
  TickType_t lastFlush = xTaskGetTickCount();
  for (;;)
  {
    if (xQueueReceive(s_q, line, pdMS_TO_TICKS(s_rp_on ? 40 : 500)) == pdTRUE && s_state == 1)
    {
      if (line[0] == CMD_BUF_APPEND)      bufAppend(line + 1);
      else if (line[0] == CMD_RP_START)   bufReplayStart();
      else if (line[0] == CMD_RP_STOP)    bufReplayStop();
      else if (line[0] == CMD_EVENT)      eventAppend(line + 1);
      else if (fputs(line, s_file) < 0 || fputc('\n', s_file) < 0) s_state = 2;
      else s_rows++;
    }
    if (s_state == 1 && s_rp_on && s_rp_state == 1) bufReplayRead();
    if (s_state == 1 && (xTaskGetTickCount() - lastFlush) >= pdMS_TO_TICKS(SDLOG_FLUSH_MS))
    {
      if (fflush(s_file) != 0) s_state = 2;
      else fsync(fileno(s_file));
      if (s_bf != NULL && s_buf_dirty) { fflush(s_bf); fsync(fileno(s_bf)); s_buf_dirty = false; }
      if (s_ef != NULL && s_ev_dirty)  { fflush(s_ef); fsync(fileno(s_ef)); s_ev_dirty = false; }
      lastFlush = xTaskGetTickCount();
    }
  }
}

bool sdlog_begin(void)
{
  esp_vfs_fat_sdmmc_mount_config_t mount_config =
  {
    .format_if_mount_failed = false,        // never wipe a card
    .max_files = 5,                         // log + event log + offline buffer + diagnostic capture + spare
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

  if (stat(BUF_PATH, &st) == 0 && st.st_size > 0)   // lines left over from before a reboot are replayed when the app connects
  {
    s_buf_bytes = (unsigned long)st.st_size;
    s_buf_pending = true;
    printf("SD: %lu bytes of undelivered BLE data found in BLEBUF.TXT\n", s_buf_bytes);
  }

  s_ef = fopen(EVENT_PATH, "a");                  // event log; a failure only means no event log
  if (s_ef != NULL) { fseek(s_ef, 0, SEEK_END); s_ev_bytes = (unsigned long)ftell(s_ef); }
  else printf("SD: cannot open SYSLOG.TXT (no event log)\n");

  // The queue holds 40 lines of 400 bytes = 16 KB. Its storage goes to PSRAM so the scarce internal RAM stays free
  // (falls back to internal RAM if PSRAM is not available).
  static StaticQueue_t s_qstruct;
  uint8_t *qmem = (uint8_t *)heap_caps_malloc((size_t)SDLOG_QUEUE_LEN * SDLOG_LINE_MAX, MALLOC_CAP_SPIRAM);
  if (qmem != NULL) s_q = xQueueCreateStatic(SDLOG_QUEUE_LEN, SDLOG_LINE_MAX, qmem, &s_qstruct);
  else s_q = xQueueCreate(SDLOG_QUEUE_LEN, SDLOG_LINE_MAX);
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

// ---- offline buffer: calls made from the loop task ----
static bool bufSend(char cmd, const char *text)
{
  if (s_state != 1 || s_q == NULL) return false;
  char tmp[SDLOG_LINE_MAX];
  tmp[0] = cmd;
  if (text) { strncpy(tmp + 1, text, SDLOG_LINE_MAX - 2); tmp[SDLOG_LINE_MAX - 1] = '\0'; }
  else tmp[1] = '\0';
  return xQueueSend(s_q, tmp, 0) == pdTRUE;
}

bool sdlog_buf_append(const char *line) { return bufSend(CMD_BUF_APPEND, line); }
bool sdlog_buf_pending(void) { return s_buf_pending; }
bool sdlog_buf_replay(bool start)
{
  if (start) s_rp_state = 0;
  return bufSend(start ? CMD_RP_START : CMD_RP_STOP, NULL);
}

int sdlog_buf_replay_get(char *out, size_t n)
{
  int st = s_rp_state;
  if (st == 3) return -1;
  if (st == 2)
  {
    __sync_synchronize();
    strncpy(out, s_rp_line, n - 1); out[n - 1] = '\0';
    s_rd_pos = s_rp_next;                          // the line is taken; next read starts after it
    s_rp_state = 1;                                // ask for the next line right away so it is ready at the next tick
    return 1;
  }
  if (st == 0) s_rp_state = 1;                     // ask the writer task for the next line
  return 0;
}

void sdlog_buf_info(unsigned long *bytes, unsigned long *dropped) { *bytes = s_buf_bytes; *dropped = s_buf_dropped; }
unsigned long sdlog_buf_skipped(void) { return s_buf_skipped; }
int sdlog_state(void) { return s_state; }

void sdlog_event(const char *line) { bufSend(CMD_EVENT, line); }
