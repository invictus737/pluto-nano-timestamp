#ifndef SHIM_RTEMS_IRQ_EXTENSION_H
#define SHIM_RTEMS_IRQ_EXTENSION_H
#include <rtems.h>
#include <sched.h>
#ifdef __cplusplus
extern "C" {
#endif
rtems_status_code rtems_interrupt_handler_install(rtems_vector_number vector, const char *info,
                                                  rtems_option options,
                                                  rtems_interrupt_handler handler, void *arg);
rtems_status_code rtems_interrupt_handler_remove(rtems_vector_number vector,
                                                 rtems_interrupt_handler handler, void *arg);
rtems_status_code rtems_interrupt_set_affinity(rtems_vector_number vector, size_t affinity_size,
                                               const cpu_set_t *affinity);
#ifdef __cplusplus
}
#endif
#endif
