/*
 * K230 BSP <tick.h> for the AIC8800 port: microsecond CPU time.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_TICK_H
#define AICWF_TICK_H
#include <stdint.h>
uint64_t cpu_ticks_us(void);
#endif
