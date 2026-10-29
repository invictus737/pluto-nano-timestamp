/*
 * Network firmware update: `fwupdate [port] [reboot]` accepts one TCP connection, reads a FIT
 * image (rtos.itb / rtos-nexus2.itb) until EOF, checks it, writes it to QSPI at 0x200000 (the
 * slot U-Boot boots with fit_config config-2) and points fit_size at it. From the Mac:
 *   nc <board> 4444 < rtos.itb
 *
 * The slot is 0x200000..0x2000000 (30 MiB) and crosses the 16 MiB bank boundary; U-Boot reads it
 * whole (rtos/QSPI.md). The FIT header (magic, totalsize) is checked on the first bytes; the
 * image is then received into a RAM buffer of its own size, and only a complete image is
 * written. If that buffer cannot be allocated the image is streamed to flash 64 KiB at a time
 * (a broken connection then leaves a broken slot and the old fit_size, which U-Boot's iminfo
 * rejects). Writing goes sector by sector: a sector whose flash content already equals the image
 * is neither erased nor programmed; every programmed page is verified by the QSPI driver. Once a
 * changed sector is erased the old image is gone, so a failure leaves U-Boot (separate
 * partition) to reload one over DFU or serial.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rtems.h>
#include <rtems/shell.h>

#include "config_store.h"
#include "qspi_nor.h"

#define FW_MAX (QSPI_NOR_FW_END - QSPI_NOR_FW_OFF)
#define FW_HDR 64u
#define FW_TAIL_MAX 0x10000u /* bytes accepted after the FIT totalsize (e.g. an appended md5) */

typedef struct {
  uint8_t *old; /* one sector of current flash content */
  unsigned long written, skipped;
} fw_writer_t;

static uint32_t be32(const uint8_t *p)
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* recv until n bytes or EOF; returns the byte count, -1 on error */
static ssize_t recv_full(int s, uint8_t *p, size_t n)
{
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(s, p + got, n - got, 0);
    if (r == 0) {
      break;
    }
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    got += (size_t)r;
  }
  return (ssize_t)got;
}

/* one 64 KiB-aligned sector: skip when unchanged, else erase + program (verified) */
static int fw_sector(fw_writer_t *w, uint32_t addr, const uint8_t *data, size_t n)
{
  if (qspi_nor_read(addr, w->old, n) == 0 && memcmp(w->old, data, n) == 0) {
    w->skipped++;
    return 0;
  }
  if (qspi_nor_erase_sector(addr) != 0 || qspi_nor_write(addr, data, n) != 0) {
    printf("fwupdate: sector %08lx failed\n", (unsigned long)addr);
    return -1;
  }
  w->written++;
  return 0;
}

static void fw_progress(size_t done)
{
  if (done % 0x100000u == 0) {
    printf("fwupdate: %lu MiB\n", (unsigned long)(done >> 20));
  }
}

static int fw_write(const uint8_t *img, size_t len, fw_writer_t *w)
{
  int rc = 0;
  for (size_t off = 0; rc == 0 && off < len; off += QSPI_NOR_SECTOR) {
    size_t n = len - off < QSPI_NOR_SECTOR ? len - off : QSPI_NOR_SECTOR;
    rc = fw_sector(w, QSPI_NOR_FW_OFF + (uint32_t)off, img + off, n);
    fw_progress(off + QSPI_NOR_SECTOR);
  }
  return rc;
}

static int fw_listen(int port)
{
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) {
    return -1;
  }
  int one = 1;
  (void)setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 1) != 0) {
    printf("fwupdate: port %d: %s\n", port, strerror(errno));
    close(ls);
    return -1;
  }
  printf("fwupdate: waiting on TCP port %d (120 s)\n", port);
  struct pollfd p = {.fd = ls, .events = POLLIN};
  if (poll(&p, 1, 120000) <= 0) {
    printf("fwupdate: nobody connected\n");
    close(ls);
    return -1;
  }
  int s = accept(ls, NULL, NULL);
  close(ls);
  if (s >= 0) {
    struct timeval tv = {.tv_sec = 20};
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  return s;
}

/* buffered: the whole image in RAM (cap bytes) before the first erase. *len = bytes to flash. */
static int fw_buffered(int s, const uint8_t *hdr, uint32_t total, uint8_t *buf, size_t cap,
                       size_t *len)
{
  memcpy(buf, hdr, FW_HDR);
  ssize_t r = recv_full(s, buf + FW_HDR, cap - FW_HDR);
  if (r < 0) {
    printf("fwupdate: receive: %s: nothing written\n", strerror(errno));
    return -1;
  }
  size_t have = FW_HDR + (size_t)r;
  if (have == cap) {
    uint8_t probe;
    if (recv(s, &probe, 1, 0) != 0) {
      printf("fwupdate: more than %lu bytes or no EOF (FIT totalsize %lu): nothing written\n",
             (unsigned long)cap, (unsigned long)total);
      return -1;
    }
  }
  if (have < total) {
    printf("fwupdate: %lu of %lu bytes received: nothing written\n", (unsigned long)have,
           (unsigned long)total);
    return -1;
  }
  *len = have;
  return 0;
}

/* streamed: no RAM for the whole image, 64 KiB at a time straight to flash */
static int fw_streamed(int s, const uint8_t *hdr, uint32_t total, fw_writer_t *w, size_t *len)
{
  uint8_t *sec = malloc(QSPI_NOR_SECTOR);
  if (sec == NULL) {
    printf("fwupdate: no memory\n");
    return -1;
  }
  memcpy(sec, hdr, FW_HDR);
  size_t fill = FW_HDR, off = 0;
  int rc = 0;
  printf("fwupdate: no RAM for the whole image, streaming to flash\n");
  for (;;) {
    ssize_t r = recv_full(s, sec + fill, QSPI_NOR_SECTOR - fill);
    if (r < 0) {
      printf("fwupdate: receive: %s after %lu bytes\n", strerror(errno),
             (unsigned long)(off + fill));
      rc = -1;
      break;
    }
    fill += (size_t)r;
    if (fill == 0) {
      break;
    }
    if (off + fill > FW_MAX) {
      printf("fwupdate: image larger than the %u byte slot\n", (unsigned)FW_MAX);
      rc = -1;
      break;
    }
    rc = fw_sector(w, QSPI_NOR_FW_OFF + (uint32_t)off, sec, fill);
    off += fill;
    fw_progress(off);
    if (rc != 0 || fill < QSPI_NOR_SECTOR) {
      break; /* error, or a short sector = EOF */
    }
    fill = 0;
  }
  free(sec);
  if (rc == 0 && off < total) {
    printf("fwupdate: only %lu of %lu bytes received\n", (unsigned long)off,
           (unsigned long)total);
    rc = -1;
  }
  *len = off;
  return rc;
}

static int shell_fwupdate(int argc, char **argv)
{
  int port = 4444, reboot = 0;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "reboot") == 0) {
      reboot = 1;
    } else {
      port = atoi(argv[i]);
    }
  }
  int s = fw_listen(port);
  if (s < 0) {
    return 1;
  }
  /* FIT = flattened device tree: magic d00dfeed, totalsize at offset 4 */
  uint8_t hdr[FW_HDR];
  ssize_t h = recv_full(s, hdr, FW_HDR);
  uint32_t magic = h == (ssize_t)FW_HDR ? be32(hdr) : 0;
  uint32_t total = h == (ssize_t)FW_HDR ? be32(hdr + 4) : 0;
  if (magic != 0xd00dfeedu || total < FW_HDR || total > FW_MAX) {
    printf("fwupdate: not a FIT image for the %u byte slot (magic %08lx, size %lu): nothing "
           "written\n",
           (unsigned)FW_MAX, (unsigned long)magic, (unsigned long)total);
    close(s);
    return 1;
  }
  fw_writer_t w = {.old = malloc(QSPI_NOR_SECTOR)};
  if (w.old == NULL) {
    printf("fwupdate: no memory\n");
    close(s);
    return 1;
  }
  size_t cap = (size_t)total + FW_TAIL_MAX > FW_MAX ? FW_MAX : (size_t)total + FW_TAIL_MAX;
  uint8_t *img = malloc(cap);
  size_t len = 0;
  int rc; /* 0 written, 1 nothing written, 2 slot changed and broken */
  if (img != NULL) {
    rc = fw_buffered(s, hdr, total, img, cap, &len) == 0 ? 0 : 1;
    close(s);
    if (rc == 0) {
      printf("fwupdate: %lu bytes received, writing to QSPI 0x%x\n", (unsigned long)len,
             (unsigned)QSPI_NOR_FW_OFF);
      qspi_nor_fw_window(1);
      rc = fw_write(img, len, &w) == 0 ? 0 : 2;
      qspi_nor_fw_window(0);
    }
    free(img);
  } else {
    qspi_nor_fw_window(1);
    rc = fw_streamed(s, hdr, total, &w, &len) == 0 ? 0 : (w.written ? 2 : 1);
    qspi_nor_fw_window(0);
    close(s);
  }
  free(w.old);
  if (rc == 1) {
    return 1;
  }
  if (rc == 2) {
    printf("fwupdate: FLASH WRITE FAILED (%lu sectors changed): reload an image from U-Boot "
           "(DFU or serial loady)\n",
           w.written);
    return 1;
  }
  printf("fwupdate: %lu sectors written, %lu unchanged\n", w.written, w.skipped);
  if (QSPI_NOR_FW_OFF + len > QSPI_NOR_LOWER_BANK_END) {
    printf("fwupdate: image crosses 16 MiB: the Linux boot copy and UBI data are overwritten\n");
  }
  char size_hex[16];
  snprintf(size_hex, sizeof(size_hex), "%lX", (unsigned long)len);
  const char *kv[] = {"fit_size", size_hex, "fit_config", "config-2"};
  if (nexus_uboot_env_set(kv, 2) != 0) {
    printf("fwupdate: image written but fit_size not set (set fit_size %s in U-Boot)\n", size_hex);
    return 1;
  }
  printf("fwupdate: OK, fit_size %s%s\n", size_hex, reboot ? ", rebooting" : "; reboot to use it");
  if (reboot) {
    sleep(1);
    rtems_fatal(RTEMS_FATAL_SOURCE_EXIT, 0); /* the fatal extension resets the board */
  }
  return 0;
}

/* `reboot [dfu]`: plain reset, or a one-shot boot into U-Boot's DFU (dfu_sf) without the serial
 * console. bootcmd gets a guard once (backup of the previous value in nexus_bootcmd_orig): when
 * nexus_boot_dfu is set, U-Boot clears it, saves, and enters DFU; otherwise it boots as before. */
#define DFU_GUARD "if test -n \"${nexus_boot_dfu}\"; then setenv nexus_boot_dfu; saveenv; run dfu_sf; fi; "
static int shell_reboot(int argc, char **argv)
{
  if (argc >= 2 && strcmp(argv[1], "dfu") == 0) {
    char *bc = nexus_uboot_env_get("bootcmd");
    if (bc == NULL) {
      printf("reboot dfu: U-Boot environment unreadable, nothing changed\n");
      return 1;
    }
    int ok;
    if (strstr(bc, "nexus_boot_dfu") != NULL) {
      const char *kv[] = {"nexus_boot_dfu", "1"};
      ok = nexus_uboot_env_set(kv, 1) == 0;
    } else {
      size_t n = strlen(DFU_GUARD) + strlen(bc) + 1;
      char *nb = malloc(n);
      if (nb == NULL) {
        free(bc);
        return 1;
      }
      snprintf(nb, n, "%s%s", DFU_GUARD, bc);
      const char *kv[] = {"nexus_bootcmd_orig", bc, "bootcmd", nb, "nexus_boot_dfu", "1"};
      ok = nexus_uboot_env_set(kv, 3) == 0;
      free(nb);
    }
    free(bc);
    if (!ok) {
      printf("reboot dfu: U-Boot environment not written, not rebooting\n");
      return 1;
    }
    printf("reboot dfu: next boot enters DFU (dfu-util -a firmware.dfu -D image.itb), resetting\n");
  } else {
    printf("reboot: resetting\n");
  }
  sleep(1);
  rtems_fatal(RTEMS_FATAL_SOURCE_EXIT, 0); /* the fatal extension resets the board */
  return 0;
}

void nexus_fwupdate_register(void)
{
  rtems_shell_add_cmd("fwupdate", "misc",
                      "fwupdate [port] [reboot]: receive a FIT image over TCP and flash it",
                      shell_fwupdate);
  rtems_shell_add_cmd("reboot", "misc", "reboot [dfu]: reset, or boot once into U-Boot DFU", shell_reboot);
}
