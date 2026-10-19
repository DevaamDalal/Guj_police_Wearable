#ifndef SDLOG_BSP_H
#define SDLOG_BSP_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SDLOG_LINE_MAX 400

bool sdlog_begin(void);                          // mount card, create next LOGxxxx.TXT, start writer task
const char *sdlog_last_error(void);              // why sdlog_begin() failed (empty if it did not)
bool sdlog_active(void);                         // true while the file is open and writable
void sdlog_write(const char *line);              // non-blocking; copies the line, drops it if the queue is full
void sdlog_status(char *out, size_t n);          // e.g. "SD LOG0003  4521 rows"

// Secondary file for one-off captures (FIFO diagnostic). Opened next to the log, written from the caller's task.
bool sdlog_aux_open(const char *prefix, const char *ext, char *nameOut, size_t n);   // PREFIXnnnn.EXT, false if no card
bool sdlog_aux_write(const char *text);          // false on write error
void sdlog_aux_close(void);

// Event log for diagnostics: one line per event appended to SYSLOG.TXT (kept across boots). Never blocks; dropped if the queue is full.
void sdlog_event(const char *line);

// Offline buffer ("blackout buffer"): packets that could not be sent over BLE are appended to BLEBUF.TXT (one line each)
// and replayed, oldest first, after the app reconnects. All file work happens in the SD writer task; these calls never block.
bool sdlog_buf_append(const char *line);         // queue one line for BLEBUF.TXT; false if the queue is full / no card
bool sdlog_buf_pending(void);                    // true while BLEBUF.TXT holds lines that were not delivered yet
bool sdlog_buf_replay(bool start);               // true = begin/resume replay, false = pause it (link lost). false return = retry
int  sdlog_buf_replay_get(char *out, size_t n);  // 1 = line returned, 0 = nothing yet (call again), -1 = replay finished (file deleted)
void sdlog_buf_info(unsigned long *bytes, unsigned long *dropped);
unsigned long sdlog_buf_skipped(void);           // damaged lines in BLEBUF.TXT that were skipped instead of sent
int  sdlog_state(void);                          // 0 = no card, 1 = logging, 2 = write error

#ifdef __cplusplus
}
#endif
#endif
