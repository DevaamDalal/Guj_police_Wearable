#ifndef TOUCH_BSP_H
#define TOUCH_BSP_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_SERIAL_DEBUG 1   // print "TOUCH x y" once per press (set 0 to keep Serial log clean)

bool touch_init(void);                          // call after I2Cbus.begin() and g_i2cMux creation
bool touch_read(uint16_t *x, uint16_t *y);      // true = finger down (raw panel coords)
void touch_get_last(int *x, int *y, bool *down);

#ifdef __cplusplus
}
#endif
#endif
