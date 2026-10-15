#ifndef RTC_BSP_H
#define RTC_BSP_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int year, month, day, hour, min, sec; } RtcTime;

bool rtc_get(RtcTime *t);        // false on I2C error or if the RTC lost power (VL flag set)
bool rtc_set(const RtcTime *t);  // writes time and clears the VL flag

#ifdef __cplusplus
}
#endif
#endif
