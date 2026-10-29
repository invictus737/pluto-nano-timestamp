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
 * - on the Pluto Nano its interrupt-driven zqspi_transfer() never completes (the QSPI interrupt
 *   does not arrive): every transfer waited for the driver timeout, the JEDEC id read back as
 *   000000 and boot lost 10 s.
 * So the controller is driven here directly in I/O mode, polled, one byte per FIFO entry, with
 * manual chip select - the way U-Boot's zynq_qspi driver uses it on this board.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "nexus_mutex.h"
#include "qspi_nor.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rtems.h>

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

/* Zynq-7000 QSPI controller (TRM ch. 12), I/O mode */
#define QSPI_BASE 0xe000d000U
#define QSPI_CR (QSPI_BASE + 0x00)
#define QSPI_ISR (QSPI_BASE + 0x04)
#define QSPI_IDR (QSPI_BASE + 0x0c)
#define QSPI_EN (QSPI_BASE + 0x14)
#define QSPI_RXD (QSPI_BASE + 0x20)
#define QSPI_TX_THRES (QSPI_BASE + 0x28)
#define QSPI_RX_THRES (QSPI_BASE + 0x2c)
#define QSPI_TXD1 (QSPI_BASE + 0x80)
#define QSPI_LQSPI_CR (QSPI_BASE + 0xa0)
#define CR_IFMODE (1U << 31)    /* flash interface mode */
#define CR_HOLDB_DR (1U << 19)  /* drive HOLD#/WP# high */
#define CR_SSFORCE (1U << 14)   /* manual chip select */
#define CR_PCS (1U << 10)       /* chip select line, 1 = deasserted */
#define CR_FIFO_32 (3U << 6)
#define CR_BAUD_SHIFT 3
#define CR_MSTREN (1U << 0)
#define ISR_RXNEMPTY (1U << 4)
#define POLL_LIMIT 200000u      /* ~ms at most per byte; a stuck controller fails, not hangs */

NEXUS_PI_MUTEX(lock)
static int ready;
static uint32_t jedec;
static uint8_t verify_buf[PAGE];
static uint32_t baud_field = 7;
static int burst_ok; /* batched reads matched byte-by-byte reads at init */
static void burst_selftest(void);

static void controller_setup(void)
{
  REG32(QSPI_EN) = 0;
  REG32(QSPI_LQSPI_CR) &= ~(1U << 31); /* linear (XIP) mode off */
  REG32(QSPI_IDR) = 0x7fU;              /* polled: no interrupts */
  /* U-Boot leaves the RX threshold at 32 entries: RX_NOT_EMPTY would never rise for the
   * one-byte entries used here */
  REG32(QSPI_RX_THRES) = 1;
  REG32(QSPI_TX_THRES) = 1;
  REG32(QSPI_CR) = CR_IFMODE | CR_HOLDB_DR | CR_SSFORCE | CR_PCS | CR_FIFO_32 |
                   (baud_field << CR_BAUD_SHIFT) | CR_MSTREN;
  REG32(QSPI_EN) = 1;
  printf("qspi: cr %08lx rx_thres %lu\n", (unsigned long)REG32(QSPI_CR),
         (unsigned long)REG32(QSPI_RX_THRES));
  while (REG32(QSPI_ISR) & ISR_RXNEMPTY) {
    (void)REG32(QSPI_RXD);
  }
}

static int byte_xfer(uint8_t out, uint8_t *in)
{
  REG32(QSPI_TXD1) = out; /* 1-byte FIFO entry, auto start */
  for (unsigned i = 0; (REG32(QSPI_ISR) & ISR_RXNEMPTY) == 0; ++i) {
    if (i > POLL_LIMIT) {
      return -1;
    }
  }
  uint32_t v = REG32(QSPI_RXD);
  if (in) {
    *in = (uint8_t)(v >> 24); /* a 1-byte entry lands in the top byte */
  }
  return 0;
}

/* Bulk receive: queue up to BURST one-byte dummy entries, then collect their RX entries. The
 * byte-by-byte path waits for every byte's round trip through the shifter; the JFFS2 mount reads
 * the whole 896 KiB region that way. TX and RX FIFOs are 63 entries deep. */
#define BURST 32u
static int rx_burst(uint8_t *rx, size_t n)
{
  while (n) {
    unsigned k = n > BURST ? BURST : (unsigned)n;
    for (unsigned i = 0; i < k; ++i) {
      REG32(QSPI_TXD1) = 0xff;
    }
    for (unsigned i = 0; i < k; ++i) {
      for (unsigned w = 0; (REG32(QSPI_ISR) & ISR_RXNEMPTY) == 0; ++w) {
        if (w > POLL_LIMIT) {
          return -1;
        }
      }
      rx[i] = (uint8_t)(REG32(QSPI_RXD) >> 24);
    }
    rx += k;
    n -= k;
  }
  return 0;
}

/* one SPI transaction under one chip select: cmd bytes, tx bytes, then rx_len bytes clocked in */
static int xfer(const uint8_t *cmd, size_t cmd_len, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                size_t rx_len)
{
  int rc = 0;
  REG32(QSPI_CR) &= ~CR_PCS;
  for (size_t i = 0; rc == 0 && i < cmd_len; ++i) {
    rc = byte_xfer(cmd[i], NULL);
  }
  for (size_t i = 0; rc == 0 && i < tx_len; ++i) {
    rc = byte_xfer(tx[i], NULL);
  }
  if (rc == 0 && burst_ok && rx_len > 1) {
    rc = rx_burst(rx, rx_len);
  } else {
    for (size_t i = 0; rc == 0 && i < rx_len; ++i) {
      rc = byte_xfer(0xff, &rx[i]);
    }
  }
  REG32(QSPI_CR) |= CR_PCS;
  if (rc != 0) {
    /* a stalled transfer may leave entries behind: start the next one with empty FIFOs */
    while (REG32(QSPI_ISR) & ISR_RXNEMPTY) {
      (void)REG32(QSPI_RXD);
    }
  }
  return rc;
}

static int cmd_only(uint8_t op)
{
  return xfer(&op, 1, NULL, 0, NULL, 0);
}

static int read_reg(uint8_t op, uint8_t *v)
{
  return xfer(&op, 1, NULL, 0, v, 1);
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
  baud_field = n;
  REG32(QSPI_CR) = (REG32(QSPI_CR) & ~(7U << CR_BAUD_SHIFT)) | (n << CR_BAUD_SHIFT);
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
  controller_setup();
  set_sclk();
  {
    uint8_t op = 0x9f, id[3] = {0, 0, 0};
    if (xfer(&op, 1, NULL, 0, id, 3) != 0) {
      printf("qspi: controller does not answer, flash not used\n");
      pthread_mutex_unlock(&lock);
      return -1;
    }
    jedec = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
  }
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
  burst_selftest();
  ready = 1;
  pthread_mutex_unlock(&lock);
  return 0;
}

uint32_t qspi_nor_jedec(void)
{
  return jedec;
}

static int read_locked(uint32_t addr, uint8_t *p, size_t len);

/* enable batched reads only when they return exactly what byte-by-byte reads return, on the
 * U-Boot environment (never blank) and the start of /data */
static void burst_selftest(void)
{
  static uint8_t a[2048], b[2048];
  static const uint32_t where[] = {QSPI_NOR_ENV_OFF, QSPI_NOR_DATA_OFF};
  int ok = 1;
  for (unsigned i = 0; i < 2 && ok; ++i) {
    burst_ok = 0;
    ok = read_locked(where[i], a, sizeof(a)) == 0;
    burst_ok = 1;
    ok = ok && read_locked(where[i], b, sizeof(b)) == 0 && memcmp(a, b, sizeof(a)) == 0;
  }
  burst_ok = ok;
  printf("qspi: batched reads %s\n", ok ? "on" : "OFF (mismatch or stall: byte mode)");
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

static volatile int fw_open;

void qspi_nor_fw_window(int open)
{
  fw_open = open;
}

static int in_window(uint32_t addr, size_t len)
{
  if (fw_open && addr >= QSPI_NOR_FW_OFF && addr < QSPI_NOR_FW_END &&
      len <= (size_t)(QSPI_NOR_FW_END - addr)) {
    return 1;
  }
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
