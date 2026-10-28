#ifndef SHIM_RTEMS_THREAD_H
#define SHIM_RTEMS_THREAD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int opaque[8]; } rtems_mutex;
typedef struct { int opaque[8]; } rtems_binary_semaphore;
void rtems_mutex_init(rtems_mutex *mutex, const char *name);
void rtems_mutex_lock(rtems_mutex *mutex);
void rtems_mutex_unlock(rtems_mutex *mutex);
void rtems_mutex_destroy(rtems_mutex *mutex);
void rtems_binary_semaphore_init(rtems_binary_semaphore *s, const char *name);
void rtems_binary_semaphore_post(rtems_binary_semaphore *s);
void rtems_binary_semaphore_wait(rtems_binary_semaphore *s);
int rtems_binary_semaphore_wait_timed_ticks(rtems_binary_semaphore *s, uint32_t ticks);
void rtems_binary_semaphore_destroy(rtems_binary_semaphore *s);
#ifdef __cplusplus
}
#endif
#endif
