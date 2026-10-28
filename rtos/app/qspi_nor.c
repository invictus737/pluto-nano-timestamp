/*
 * Pluto Nano QSPI NOR access (see qspi_nor.h).
 *
 * Why not zqspi_read/zqspi_write/zqspi_erase of the RTEMS 6.2 BSP driver directly:
 * - its JEDEC table only knows two Spansion parts, so zqspi_init() reports
 *   ZQSPI_FLASH_INVALID_DEVICE for the W25Q256 (EF 40 19) and leaves sizes at 0;
 * - zqspi_wait_for_write() treats status bit 5 as an erase error, but on Winbond SR1 bit 5 is
 *   TB (top/bottom protect), which U-Boot's `sf protect lock 0 100000` sets: every erase would
 *   be reported as failed;
 * - zqspi_blank() does not skip the fast-read dummy byte.
 * So only zqspi_init() (interrupt handler + transfer buffer) and zqspi_transfer() are used, with
 * the same buffer conventions as the driver's own static helpers, and the command layer below.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qspi_nor.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rtems.h>
#include <dev/spi/zynq-qspi-flash.h>
#include <dev/spi/zynq-qspi-flash-defs.h>

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SLCR_ARM_PLL_CTRL 0xf8000100U
#define SLCR_DDR_PLL_CTRL 0xf8000104U
#define SLCR_IO_PLL_CTRL 0xf8000108U
#define SLCR_APER_CLK_CTRL 0xf800012cU
#define SLCR_LQSPI_CLK_CTRL 0xf800014cU
#define APER_LQSPI_CPU1X (1U << 23)
#define PS_CLK_HZ 33333333ULL
#define SCLK_MAX_HZ 25000000ULL /* I/O mode without the loopback clock: stay well below 40 MHz */

/* W25Q256 opcodes with an explicit 4-byte address (independent of ADS/EAR) */
#define OP_READ4 0x13
#define OP_PP4 0x12
#define OP_BE64K4 0xdc
#define OP_WREN 0x06
#define OP_WRDI 0x04
#define OP_RDSR1 0x05
#define OP_RDSR2 0x35
#define OP_RDSR3 0x15
#define OP_RDEAR 0xc8
#define SR1_WIP 0x01
#define SR1_WEL 0x02

#define PAGE 256u
#define READ_CHUNK 2048u /* + 5 command bytes + 3 padding < ZQSPI_FLASH_BUFFER_SIZE */

static zqspiflash dev;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int ready;
static uint32_t jedec;
static uint8_t verify_buf[PAGE];

/* one SPI transaction: cmd bytes, then tx bytes, then rx_len bytes clocked in */
static int xfer(const uint8_t *cmd, size_t cmd_len, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                size_t rx_len)
{
  zqspi_transfer_buffer *b = &dev.buf;
  size_t len = cmd_len + tx_len + rx_len;
  if (b->buffer == NULL || len == 0 || len + 4 > b->size) {
    return -1;
  }
  /* zqspi_transfer_buffer_clear() + _set_length(): right-aligned in whole 32-bit words */
  b->length = len;
  b->padding = (4 - (len & 3)) & 3;
  b->in = b->padding;
  b->out = b->padding;
  memcpy(b->buffer + b->in, cmd, cmd_len);
  b->in += cmd_len;
  if (tx_len) {
    memcpy(b->buffer + b->in, tx, tx_len);
    b->in += tx_len;
  }
  if (rx_len) {
    memset(b->buffer + b->in, 0, rx_len);
    b->in += rx_len;
  }
  b->trans_dir = rx_len ? ZQSPI_FLASH_RX_TRANS : ZQSPI_FLASH_TX_TRANS;
  b->command_len = (uint32_t)cmd_len;
  if (zqspi_transfer(b, &dev.initialised) != ZQSPI_FLASH_NO_ERROR) {
    return -1;
  }
  if (rx_len) {
    memcpy(rx, b->buffer + b->padding + cmd_len + tx_len, rx_len);
  }
  return 0;
}

static int cmd_only(uint8_t op)
{
  return xfer(&op, 1, NULL, 0, NULL, 0);
}

static int read_reg(uint8_t op, uint8_t *v)
{
  uint8_t two[2];
  /* two bytes clocked like the driver's zqspi_read_register(); the status repeats */
  if (xfer(&op, 1, NULL, 0, two, 2) != 0) {
    return -1;
  }
  *v = two[1];
  return 0;
}

static void addr4(uint8_t *c, uint8_t op, uint32_t a)
{
  c[0] = op;
  c[1] = (uint8_t)(a >> 24);
  c[2] = (uint8_t)(a >> 16);
  c[3] = (uint8_t)(a >> 8);
  c[4] = (uint8_t)a;
}

static int wait_ready(unsigned timeout_ms, unsigned sleep_us)
{
  unsigned waited_us = 0;
  for (;;) {
    uint8_t sr;
    if (read_reg(OP_RDSR1, &sr) != 0) {
      return -1;
    }
    if ((sr & SR1_WIP) == 0) {
      return 0;
    }
    if (waited_us >= timeout_ms * 1000u) {
      return -1;
    }
    usleep(sleep_us);
    waited_us += sleep_us < 1000 ? 1000 : sleep_us; /* at least one clock tick */
  }
}

static int write_enable(void)
{
  uint8_t sr;
  if (cmd_only(OP_WREN) != 0 || read_reg(OP_RDSR1, &sr) != 0) {
    return -1;
  }
  return (sr & SR1_WEL) ? 0 : -1;
}

static uint64_t pll_hz(uint32_t ctrl)
{
  return PS_CLK_HZ * ((REG32(ctrl) >> 12) & 0x7fU);
}

static uint64_t qspi_ref_hz(void)
{
  uint32_t c = REG32(SLCR_LQSPI_CLK_CTRL);
  uint32_t div = (c >> 8) & 0x3fU;
  uint32_t src = (c >> 4) & 0x3U;
  uint64_t pll = src == 2 ? pll_hz(SLCR_ARM_PLL_CTRL)
               : src == 3 ? pll_hz(SLCR_DDR_PLL_CTRL) : pll_hz(SLCR_IO_PLL_CTRL);
  return div ? pll / div : 0;
}

/* baud field [5:3]: SCLK = ref / (2 << n) */
static void set_sclk(void)
{
  uint64_t ref = qspi_ref_hz();
  uint32_t n = 7;
  if (ref != 0) {
    for (n = 0; n < 7 && ref / (2ULL << n) > SCLK_MAX_HZ; ++n) {
    }
  }
  uint32_t cr = qspi_reg_read(ZQSPI_QSPI_REG_CONFIG);
  cr = (cr & ~(7U << 3)) | (n << 3);
  qspi_reg_write(ZQSPI_QSPI_REG_CONFIG, cr);
  printf("qspi: ref %llu Hz, sclk %llu Hz\n", (unsigned long long)ref,
         (unsigned long long)(ref ? ref / (2ULL << n) : 0));
}

int qspi_nor_init(void)
{
  pthread_mutex_lock(&lock);
  if (ready) {
    pthread_mutex_unlock(&lock);
    return 0;
  }
  if ((REG32(SLCR_APER_CLK_CTRL) & APER_LQSPI_CPU1X) == 0 ||
      (REG32(SLCR_LQSPI_CLK_CTRL) & 1U) == 0) {
    printf("qspi: controller clock off, flash not used\n");
    pthread_mutex_unlock(&lock);
    return -1;
  }
  zqspi_error e = zqspi_init(&dev);
  if (e == ZQSPI_FLASH_RTEMS_INTR || dev.buf.buffer == NULL) {
    printf("qspi: driver init failed (%d)\n", (int)e);
    pthread_mutex_unlock(&lock);
    return -1;
  }
  /* the first RDID ran at the driver's ref/4: slow down and identify again */
  if (dev.initialised) {
    set_sclk();
  }
  dev.jedec_id = 0;
  (void)zqspi_readid(&dev, &jedec); /* INVALID_DEVICE for the W25Q256 (not in its table) */
  if ((jedec >> 16) != 0xef || (jedec & 0xff) != 0x19) {
    printf("qspi: unexpected JEDEC id %06lx, flash not used\n", (unsigned long)jedec);
    pthread_mutex_unlock(&lock);
    return -1;
  }
  uint8_t sr1 = 0, sr2 = 0, sr3 = 0, ear = 0;
  (void)read_reg(OP_RDSR1, &sr1);
  (void)read_reg(OP_RDSR2, &sr2);
  (void)read_reg(OP_RDSR3, &sr3);
  (void)read_reg(OP_RDEAR, &ear);
  printf("qspi: W25Q256 id %06lx sr1 %02x sr2 %02x sr3 %02x ear %02x (%s-byte mode, left as is)\n",
         (unsigned long)jedec, sr1, sr2, sr3, ear, (sr3 & 1) ? "4" : "3");
  if (sr1 & SR1_WIP) {
    (void)wait_ready(3000, 1000);
  }
  ready = 1;
  pthread_mutex_unlock(&lock);
  return 0;
}

uint32_t qspi_nor_jedec(void)
{
  return jedec;
}

static int read_locked(uint32_t addr, uint8_t *p, size_t len)
{
  while (len) {
    size_t n = len > READ_CHUNK ? READ_CHUNK : len;
    uint8_t c[5];
    addr4(c, OP_READ4, addr);
    if (xfer(c, 5, NULL, 0, p, n) != 0) {
      return -1;
    }
    addr += (uint32_t)n;
    p += n;
    len -= n;
  }
  return 0;
}

int qspi_nor_read(uint32_t addr, void *buf, size_t len)
{
  if (!ready || addr >= 0x2000000u || len > 0x2000000u - addr) {
    return -1;
  }
  pthread_mutex_lock(&lock);
  int rc = read_locked(addr, buf, len);
  pthread_mutex_unlock(&lock);
  return rc;
}

static int in_window(uint32_t addr, size_t len)
{
  return addr >= QSPI_NOR_WRITE_LO && addr < QSPI_NOR_WRITE_HI &&
         len <= (size_t)(QSPI_NOR_WRITE_HI - addr);
}

int qspi_nor_write(uint32_t addr, const void *buf, size_t len)
{
  const uint8_t *p = buf;
  if (!ready || !in_window(addr, len)) {
    printf("qspi: write %08lx+%lu refused (outside the config window)\n", (unsigned long)addr,
           (unsigned long)len);
    return -1;
  }
  pthread_mutex_lock(&lock);
  int rc = 0;
  while (len && rc == 0) {
    size_t n = PAGE - (addr % PAGE);
    if (n > len) {
      n = len;
    }
    int blank = 1;
    for (size_t i = 0; i < n && blank; ++i) {
      blank = p[i] == 0xff;
    }
    if (!blank) {
      uint8_t c[5];
      addr4(c, OP_PP4, addr);
      if (write_enable() != 0 || xfer(c, 5, p, n, NULL, 0) != 0 || wait_ready(50, 100) != 0) {
        rc = -1;
      } else if (read_locked(addr, verify_buf, n) != 0 || memcmp(verify_buf, p, n) != 0) {
        printf("qspi: verify failed at %08lx\n", (unsigned long)addr);
        rc = -1;
      }
    }
    addr += (uint32_t)n;
    p += n;
    len -= n;
  }
  if (rc != 0) {
    (void)cmd_only(OP_WRDI);
  }
  pthread_mutex_unlock(&lock);
  return rc;
}

int qspi_nor_erase_sector(uint32_t addr)
{
  if (!ready || (addr % QSPI_NOR_SECTOR) != 0 || !in_window(addr, QSPI_NOR_SECTOR)) {
    printf("qspi: erase %08lx refused\n", (unsigned long)addr);
    return -1;
  }
  pthread_mutex_lock(&lock);
  uint8_t c[5];
  int rc = 0;
  addr4(c, OP_BE64K4, addr);
  if (write_enable() != 0 || xfer(c, 5, NULL, 0, NULL, 0) != 0 || wait_ready(3000, 2000) != 0) {
    rc = -1;
  }
  /* blank check */
  for (uint32_t off = 0; rc == 0 && off < QSPI_NOR_SECTOR; off += PAGE) {
    if (read_locked(addr + off, verify_buf, PAGE) != 0) {
      rc = -1;
      break;
    }
    for (unsigned i = 0; i < PAGE; ++i) {
      if (verify_buf[i] != 0xff) {
        printf("qspi: erase verify failed at %08lx\n", (unsigned long)(addr + off + i));
        rc = -1;
        break;
      }
    }
  }
  if (rc != 0) {
    (void)cmd_only(OP_WRDI);
  }
  pthread_mutex_unlock(&lock);
  return rc;
}
