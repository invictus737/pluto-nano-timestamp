/* Compile-check shim: declarations of the RTEMS 6 API subset the Pluto driver uses, so the
 * RTEMS-only sources can be syntax/type checked with a host or Linux cross compiler. Not a
 * runtime implementation. */
#ifndef SHIM_RTEMS_H
#define SHIM_RTEMS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { RTEMS_SUCCESSFUL = 0, RTEMS_INVALID_ID = 4 } rtems_status_code;
typedef uint32_t rtems_vector_number;
typedef uint32_t rtems_interval;
typedef uint32_t rtems_option;
typedef void (*rtems_interrupt_handler)(void *);
#define RTEMS_INTERRUPT_UNIQUE ((rtems_option)0x00000001)
const char *rtems_status_text(rtems_status_code sc);
rtems_status_code rtems_task_wake_after(rtems_interval ticks);
rtems_interval rtems_clock_get_ticks_per_second(void);
rtems_interval rtems_clock_get_ticks_since_boot(void);
uint32_t rtems_scheduler_get_processor_maximum(void);
void *rtems_cache_coherent_allocate(size_t size, uintptr_t alignment, uintptr_t boundary);
void rtems_cache_coherent_free(void *ptr);
void rtems_cache_flush_multiple_data_lines(const void *addr, size_t size);
void rtems_cache_invalidate_multiple_data_lines(const void *addr, size_t size);
#ifdef __cplusplus
}
#endif
#endif
