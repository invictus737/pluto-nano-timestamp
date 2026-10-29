/*
 * Real-time placement of the Pluto driver's threads and interrupts.
 *
 * The driver runs one DMA service thread per direction ("PLRX", "PLTX"): the AXI DMAC interrupt
 * only acknowledges the controller and wakes its service thread, which reaps completed blocks,
 * resubmits transfers and wakes the stream user. Both threads are created through
 * pluto_rt_thread_create(), which builds a pthread attribute (SCHED_FIFO, explicit scheduling,
 * PLUTO_RT_PRIORITY, 16 KiB stack) and then calls the installed attribute hook, if any, right
 * before pthread_create(). That hook is the place to pin the radio to CPU1 later, e.g.
 *
 *   static void pin_cpu1(pthread_attr_t *attr, const char *name, void *arg)
 *   {
 *     cpu_set_t set;
 *     CPU_ZERO(&set);
 *     CPU_SET(1, &set);
 *     pthread_attr_setaffinity_np(attr, sizeof(set), &set);
 *   }
 *   ...
 *   pluto_rtems_set_thread_attr_hook(pin_cpu1, NULL);   // before the device is opened
 *
 * or simply pluto_rtems_pin_to_cpu(1), which installs such a hook and also routes the two DMAC
 * interrupts to that CPU (rtems_interrupt_set_affinity). The hook must be installed before
 * SoapySDR opens the device (threads start with the first setupStream). The scheduler instance
 * owning that CPU must allow the affinity (EDF SMP does).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PLUTO_RT_H
#define PLUTO_RT_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX priority of the service threads: below the maximum, above every nexus2 worker */
#define PLUTO_RT_PRIORITY_FROM_MAX 2
#define PLUTO_RT_STACK_SIZE (16 * 1024)

typedef void (*pluto_thread_attr_hook)(pthread_attr_t *attr, const char *name, void *arg);

/* install (or with NULL remove) the hook called for every driver thread */
void pluto_rtems_set_thread_attr_hook(pluto_thread_attr_hook hook, void *arg);

/* convenience: pin the driver threads and the DMAC interrupts to one CPU; 0 ok, -1 invalid */
int pluto_rtems_pin_to_cpu(int cpu);

/* create a driver thread with the RT attribute + hook; 0 ok, else errno */
int pluto_rt_thread_create(pthread_t *thread, const char *name, void *(*fn)(void *), void *arg);

/*
 * Housekeeping thread of the driver (level/overflow log): SCHED_FIFO one step above the lowest
 * POSIX priority, far below the radio and every nexus2 worker, pinned to CPU0 (away from a radio
 * pinned to CPU1); without the pin when the scheduler refuses CPU0. The attribute hook is not
 * applied. 0 ok, else errno.
 */
int pluto_rt_low_thread_create(pthread_t *thread, const char *name, void *(*fn)(void *), void *arg);

/* apply the configured interrupt CPU (if pluto_rtems_pin_to_cpu was used) to one vector */
void pluto_rt_irq_affinity(unsigned vector);

#ifdef __cplusplus
}
#endif
#endif
