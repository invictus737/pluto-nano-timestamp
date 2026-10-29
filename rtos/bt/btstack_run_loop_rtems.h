/*
 * BTstack run loop for RTEMS: one POSIX thread, a mutex + monotonic condition variable instead
 * of select() on pipes (libbsd select() only sees libbsd descriptors), BTstack's portable timer,
 * data source and callback lists. Other threads wake it with
 * btstack_run_loop_poll_data_sources_from_irq() (data sources with DATA_SOURCE_CALLBACK_POLL)
 * and btstack_run_loop_execute_on_main_thread().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef BTSTACK_RUN_LOOP_RTEMS_H
#define BTSTACK_RUN_LOOP_RTEMS_H

#include "btstack_run_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

const btstack_run_loop_t *btstack_run_loop_rtems_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif
