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

#ifdef __cplusplus
}
#endif
#endif
