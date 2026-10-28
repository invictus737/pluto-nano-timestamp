/*
 * ADI no-OS platform layer for RTEMS on the Zynq-7010 PS: SPI (Cadence SPI, polled), GPIO (PS
 * GPIO incl. EMIO banks 2/3), delays, AXI register access. Allocation comes from no-OS
 * util/no_os_alloc.c (malloc/calloc/free), mutexes from util/no_os_mutex.c (the driver
 * serialises all AD9363 access itself).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PLUTO_PLATFORM_RTEMS_H
#define PLUTO_PLATFORM_RTEMS_H

#include <stdint.h>

#include "no_os_gpio.h"
#include "no_os_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Zynq PS SPI, controller selected by no_os_spi_init_param.device_id (0 = 0xe0006000,
 * 1 = 0xe0007000). Manual chip select held for the whole transfer, auto start, FIFO polled. */
extern const struct no_os_spi_platform_ops pluto_zynq_spi_ops;

/* Zynq PS GPIO, number = Linux gpio0 numbering (MIO 0..53, EMIO 54..117) */
extern const struct no_os_gpio_platform_ops pluto_zynq_gpio_ops;

/* direct helpers for the driver (same numbering) */
void zynq_gpio_output(unsigned number, int value);
int zynq_gpio_get(unsigned number);

/* SPI reference clock as programmed by the FSBL (SLCR SPI_CLK_CTRL), 0 when stopped */
uint32_t zynq_spi_ref_hz(void);

#ifdef __cplusplus
}
#endif
#endif
