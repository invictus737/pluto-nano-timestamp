/*
 * Driver thread creation with an attribute hook (CPU pinning), see pluto_rt.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pluto_rt.h"

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rtems.h>
#include <rtems/irq-extension.h>

static pluto_thread_attr_hook g_hook;
static void *g_hook_arg;
static int g_irq_cpu = -1;

void pluto_rtems_set_thread_attr_hook(pluto_thread_attr_hook hook, void *arg)
{
  g_hook_arg = arg;
  g_hook = hook;
}

static void pin_hook(pthread_attr_t *attr, const char *name, void *arg)
{
  int cpu = (int)(intptr_t)arg;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  int rc = pthread_attr_setaffinity_np(attr, sizeof(set), &set);
  if (rc != 0)
    printf("pluto: %s: CPU%d affinity refused (%d)\n", name, cpu, rc);
}

int pluto_rtems_pin_to_cpu(int cpu)
{
  if (cpu < 0 || (uint32_t)cpu >= rtems_scheduler_get_processor_maximum())
    return -1;
  g_irq_cpu = cpu;
  pluto_rtems_set_thread_attr_hook(pin_hook, (void *)(intptr_t)cpu);
  return 0;
}

void pluto_rt_irq_affinity(unsigned vector)
{
  if (g_irq_cpu < 0)
    return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(g_irq_cpu, &set);
  rtems_status_code sc = rtems_interrupt_set_affinity((rtems_vector_number)vector, sizeof(set), &set);
  if (sc != RTEMS_SUCCESSFUL)
    printf("pluto: IRQ %u CPU%d affinity: %s\n", vector, g_irq_cpu, rtems_status_text(sc));
}

int pluto_rt_thread_create(pthread_t *thread, const char *name, void *(*fn)(void *), void *arg)
{
  pthread_attr_t attr;
  struct sched_param sp;
  int rc = pthread_attr_init(&attr);
  if (rc)
    return rc;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = sched_get_priority_max(SCHED_FIFO) - PLUTO_RT_PRIORITY_FROM_MAX;
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  pthread_attr_setschedparam(&attr, &sp);
  pthread_attr_setstacksize(&attr, PLUTO_RT_STACK_SIZE);
  if (g_hook)
    g_hook(&attr, name, g_hook_arg);
  rc = pthread_create(thread, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  if (rc == 0)
    pthread_setname_np(*thread, name);
  return rc;
}

static int low_thread_try(pthread_t *thread, void *(*fn)(void *), void *arg, int pin_cpu0)
{
  pthread_attr_t attr;
  struct sched_param sp;
  int rc = pthread_attr_init(&attr);
  if (rc)
    return rc;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = sched_get_priority_min(SCHED_FIFO) + 1;
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  pthread_attr_setschedparam(&attr, &sp);
  pthread_attr_setstacksize(&attr, PLUTO_RT_STACK_SIZE);
  if (pin_cpu0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    rc = pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
  }
  if (rc == 0)
    rc = pthread_create(thread, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  return rc;
}

int pluto_rt_low_thread_create(pthread_t *thread, const char *name, void *(*fn)(void *), void *arg)
{
  int rc = low_thread_try(thread, fn, arg, 1);
  if (rc != 0) {
    printf("pluto: %s: CPU0 pin refused (%d), unpinned\n", name, rc);
    rc = low_thread_try(thread, fn, arg, 0);
  }
  if (rc == 0)
    pthread_setname_np(*thread, name);
  return rc;
}
