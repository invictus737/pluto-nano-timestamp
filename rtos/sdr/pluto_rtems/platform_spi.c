/*
 * no-OS SPI platform ops for the Zynq-7000 PS SPI controller (Cadence SPI r1p6) on RTEMS.
 * Polled master: manual chip select (CR.Manual_CS) held low for the whole message, automatic
 * start, the 128-byte FIFOs filled/drained by the CPU. AD9363 messages are 3..10 bytes, so a
 * transfer is a few microseconds at 5..10 MHz; it is bounded by a 10 ms timeout.
 * Register layout: Zynq-7000 TRM UG585 appendix B.30 (and Linux spi-cadence.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <rtems.h>
#include <rtems/counter.h>

#include "no_os_alloc.h"
#include "no_os_error.h"
#include "platform_rtems.h"
#include "pluto_regs.h"

#define SPI_CR 0x00u
#define SPI_ISR 0x04u
#define SPI_IDR 0x0cu
#define SPI_ER 0x14u
#define SPI_DELAY 0x18u
#define SPI_TXD 0x1cu
#define SPI_RXD 0x20u
#define SPI_MODID 0xfcu

#define CR_MSTREN (1u << 0)
#define CR_CPOL (1u << 1)
#define CR_CPHA (1u << 2)
#define CR_BAUD_SHIFT 3
#define CR_BAUD_MASK (7u << 3)
#define CR_CS_SHIFT 10
#define CR_CS_MASK (0xfu << 10)
#define CR_MANUAL_CS (1u << 14)
#define CR_MODEFAIL_EN (1u << 17)

#define ISR_RX_NEMPTY (1u << 4)
#define ISR_TX_FULL (1u << 3)
#define ISR_ALL 0x7fu
#define FIFO_DEPTH 128u
#define XFER_TIMEOUT_NS 10000000u

struct zynq_spi {
  uintptr_t base;
  uint32_t cr; /* CR with the chip select deasserted */
  uint8_t cs;
};

static inline uint32_t srd(struct zynq_spi *s, uint32_t off)
{
  return *(volatile uint32_t *)(s->base + off);
}

static inline void swr(struct zynq_spi *s, uint32_t off, uint32_t v)
{
  *(volatile uint32_t *)(s->base + off) = v;
}

static uint64_t pll_hz(uint32_t ctrl)
{
  return PS_CLK_HZ * ((*(volatile uint32_t *)(uintptr_t)ctrl >> 12) & 0x7fu);
}

uint32_t zynq_spi_ref_hz(void)
{
  uint32_t c = *(volatile uint32_t *)(uintptr_t)SLCR_SPI_CLK_CTRL;
  uint32_t div = (c >> 8) & 0x3fu;
  uint32_t src = (c >> 4) & 0x3u;
  uint64_t pll = src == 2 ? pll_hz(SLCR_ARM_PLL_CTRL)
               : src == 3 ? pll_hz(SLCR_DDR_PLL_CTRL) : pll_hz(SLCR_IO_PLL_CTRL);
  return div ? (uint32_t)(pll / div) : 0;
}

static int32_t zspi_init(struct no_os_spi_desc **desc, const struct no_os_spi_init_param *param)
{
  if (param->device_id > 1 || param->chip_select > 2)
    return -EINVAL;
  uint32_t ctrl = *(volatile uint32_t *)(uintptr_t)SLCR_SPI_CLK_CTRL;
  uint32_t act = 1u << param->device_id;
  if (!(ctrl & act)) {
    /* the FSBL normally enables SPI0 (EMIO to the AD9363); enable its clock if it did not */
    *(volatile uint32_t *)(uintptr_t)SLCR_UNLOCK = SLCR_UNLOCK_KEY;
    *(volatile uint32_t *)(uintptr_t)SLCR_SPI_CLK_CTRL = ctrl | act;
    *(volatile uint32_t *)(uintptr_t)SLCR_LOCK = SLCR_LOCK_KEY;
    printf("pluto: SPI%u reference clock was off, enabled\n", (unsigned)param->device_id);
  }
  uint32_t ref = zynq_spi_ref_hz();
  if (ref == 0)
    return -ENODEV;
  struct no_os_spi_desc *d = no_os_calloc(1, sizeof(*d));
  struct zynq_spi *s = no_os_calloc(1, sizeof(*s));
  if (!d || !s) {
    no_os_free(d);
    no_os_free(s);
    return -ENOMEM;
  }
  s->base = param->device_id ? 0xe0007000u : PLUTO_SPI_BASE;
  s->cs = param->chip_select;
  if ((srd(s, SPI_MODID) & 0xffffffu) != 0x090106u)
    printf("pluto: SPI%u module ID 0x%08x (expected 0x00090106)\n", (unsigned)param->device_id,
           (unsigned)srd(s, SPI_MODID));
  /* SCLK = ref / 2^(baud+1), baud 1..7 (Linux cdns_spi_config_clock_freq) */
  uint32_t baud = 1;
  while (baud < 7 && (ref >> (baud + 1)) > param->max_speed_hz)
    baud++;
  uint32_t cr = CR_MSTREN | CR_MANUAL_CS | CR_MODEFAIL_EN | CR_CS_MASK | (baud << CR_BAUD_SHIFT);
  if (param->mode & NO_OS_SPI_CPOL)
    cr |= CR_CPOL;
  if (param->mode & NO_OS_SPI_CPHA)
    cr |= CR_CPHA;
  s->cr = cr;
  swr(s, SPI_ER, 0);
  swr(s, SPI_IDR, ISR_ALL);
  for (unsigned i = 0; i < FIFO_DEPTH && (srd(s, SPI_ISR) & ISR_RX_NEMPTY); ++i)
    (void)srd(s, SPI_RXD);
  swr(s, SPI_ISR, ISR_ALL);
  swr(s, SPI_CR, cr);
  swr(s, SPI_ER, 1);
  printf("pluto: SPI%u CS%u mode %u at %u Hz (ref %u Hz)\n", (unsigned)param->device_id,
         (unsigned)s->cs, (unsigned)param->mode, (unsigned)(ref >> (baud + 1)), (unsigned)ref);
  d->device_id = param->device_id;
  d->max_speed_hz = param->max_speed_hz;
  d->chip_select = param->chip_select;
  d->mode = param->mode;
  d->bit_order = param->bit_order;
  d->extra = s;
  *desc = d;
  return 0;
}

static int32_t zspi_write_and_read(struct no_os_spi_desc *desc, uint8_t *data, uint16_t len)
{
  struct zynq_spi *s = desc->extra;
  int32_t ret = 0;
  /* drain stale RX bytes, then assert CS (CS field: 0 = selected, one-hot low) */
  while (srd(s, SPI_ISR) & ISR_RX_NEMPTY)
    (void)srd(s, SPI_RXD);
  swr(s, SPI_CR, (s->cr & ~CR_CS_MASK) | ((0xfu & ~(1u << s->cs)) << CR_CS_SHIFT));
  uint16_t done = 0;
  while (done < len) {
    uint32_t left = (uint32_t)len - done;
    uint16_t chunk = (uint16_t)(left > FIFO_DEPTH ? FIFO_DEPTH : left);
    for (uint16_t i = 0; i < chunk; ++i)
      swr(s, SPI_TXD, data[done + i]);
    rtems_counter_ticks t0 = rtems_counter_read();
    rtems_counter_ticks limit = rtems_counter_nanoseconds_to_ticks(XFER_TIMEOUT_NS);
    for (uint16_t i = 0; i < chunk;) {
      if (srd(s, SPI_ISR) & ISR_RX_NEMPTY) {
        data[done + i] = (uint8_t)srd(s, SPI_RXD);
        ++i;
      } else if (rtems_counter_difference(rtems_counter_read(), t0) > limit) {
        ret = -ETIMEDOUT;
        break;
      }
    }
    if (ret)
      break;
    done = (uint16_t)(done + chunk);
  }
  swr(s, SPI_CR, s->cr); /* deassert CS */
  if (ret)
    printf("pluto: SPI transfer timeout (%u bytes)\n", (unsigned)len);
  return ret;
}

static int32_t zspi_remove(struct no_os_spi_desc *desc)
{
  if (!desc)
    return -EINVAL;
  struct zynq_spi *s = desc->extra;
  if (s) {
    swr(s, SPI_ER, 0);
    no_os_free(s);
  }
  no_os_free(desc);
  return 0;
}

const struct no_os_spi_platform_ops pluto_zynq_spi_ops = {
  .init = zspi_init,
  .write_and_read = zspi_write_and_read,
  .remove = zspi_remove,
};
