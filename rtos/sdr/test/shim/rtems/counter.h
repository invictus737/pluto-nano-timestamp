#ifndef SHIM_RTEMS_COUNTER_H
#define SHIM_RTEMS_COUNTER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef uint32_t rtems_counter_ticks;
rtems_counter_ticks rtems_counter_read(void);
rtems_counter_ticks rtems_counter_difference(rtems_counter_ticks second, rtems_counter_ticks first);
rtems_counter_ticks rtems_counter_nanoseconds_to_ticks(uint32_t nanoseconds);
void rtems_counter_delay_nanoseconds(uint32_t nanoseconds);
#ifdef __cplusplus
}
#endif
#endif
