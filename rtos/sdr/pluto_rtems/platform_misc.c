/*
 * no-OS delay, time and AXI register access on RTEMS.
 * Microsecond delays (AD9363 SPI/calibration settling) busy-wait on the CPU counter; anything of
 * a clock tick or more sleeps, so long calibrations do not hold a CPU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <time.h>

#include <rtems.h>
#include <rtems/counter.h>

#include "no_os_axi_io.h"
#include "no_os_delay.h"

void no_os_udelay(uint32_t usecs)
{
  uint32_t tps = rtems_clock_get_ticks_per_second();
  uint32_t tick_us = tps ? 1000000u / tps : 1000u;
  if (usecs >= 2 * tick_us) {
    /* round up: never shorter than asked */
    rtems_task_wake_after((usecs + tick_us - 1) / tick_us + 1);
    return;
  }
  rtems_counter_delay_nanoseconds(usecs * 1000u);
}

void no_os_mdelay(uint32_t msecs)
{
  while (msecs > 1000) {
    no_os_udelay(1000000u);
    msecs -= 1000;
  }
  no_os_udelay(msecs * 1000u);
}

struct no_os_time no_os_get_time(void)
{
  struct timespec ts;
  struct no_os_time t;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  t.s = (unsigned int)ts.tv_sec;
  t.us = (unsigned int)(ts.tv_nsec / 1000);
  return t;
}

int32_t no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *data)
{
  *data = *(volatile uint32_t *)(uintptr_t)(base + offset);
  return 0;
}

int32_t no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t data)
{
  *(volatile uint32_t *)(uintptr_t)(base + offset) = data;
  return 0;
}
