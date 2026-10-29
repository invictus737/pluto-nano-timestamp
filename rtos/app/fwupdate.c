/*
 * Network firmware update: `fwupdate [port] [reboot]` accepts one TCP connection, reads a FIT
 * image (rtos.itb / rtos-nexus2.itb) until EOF, checks it, writes it to QSPI at 0x200000 (the
 * slot U-Boot boots with fit_config config-2) and points fit_size at it. From the Mac:
 *   nc <board> 4444 < rtos.itb
 * Every page is verified by the QSPI driver; the old image is gone once erasing starts, so a
 * failure leaves U-Boot (separate partition) to reload one over serial or DFU.
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

static int fw_receive(int port, uint8_t **out, size_t *out_len)
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
  if (s < 0) {
    return -1;
  }
  struct timeval tv = {.tv_sec = 20};
  (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  uint8_t *buf = malloc(FW_MAX);
  size_t len = 0;
  if (buf == NULL) {
    close(s);
    return -1;
  }
  for (;;) {
    ssize_t n = recv(s, buf + len, FW_MAX - len, 0);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      printf("fwupdate: receive: %s after %lu bytes\n", strerror(errno), (unsigned long)len);
      free(buf);
      close(s);
      return -1;
    }
    len += (size_t)n;
    if (len == FW_MAX) {
      printf("fwupdate: image larger than %u bytes\n", (unsigned)FW_MAX);
      free(buf);
      close(s);
      return -1;
    }
  }
  close(s);
  *out = buf;
  *out_len = len;
  return 0;
}

static int fw_write(const uint8_t *img, size_t len)
{
  uint32_t end = QSPI_NOR_FW_OFF + (uint32_t)len;
  int rc = 0;
  qspi_nor_fw_window(1);
  for (uint32_t a = QSPI_NOR_FW_OFF; rc == 0 && a < end; a += QSPI_NOR_SECTOR) {
    rc = qspi_nor_erase_sector(a);
  }
  printf("fwupdate: erased, writing\n");
  for (size_t off = 0; rc == 0 && off < len; off += 0x10000) {
    size_t n = len - off < 0x10000 ? len - off : 0x10000;
    rc = qspi_nor_write(QSPI_NOR_FW_OFF + (uint32_t)off, img + off, n);
  }
  qspi_nor_fw_window(0);
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
  uint8_t *img = NULL;
  size_t len = 0;
  if (fw_receive(port, &img, &len) != 0) {
    return 1;
  }
  /* FIT = flattened device tree: magic d00dfeed, totalsize at offset 4 */
  uint32_t magic = (uint32_t)img[0] << 24 | img[1] << 16 | img[2] << 8 | img[3];
  uint32_t total = (uint32_t)img[4] << 24 | img[5] << 16 | img[6] << 8 | img[7];
  if (len < 64 || magic != 0xd00dfeedu || total > len) {
    printf("fwupdate: %lu bytes, not a FIT image (magic %08lx, size %lu): nothing written\n",
           (unsigned long)len, (unsigned long)magic, (unsigned long)total);
    free(img);
    return 1;
  }
  printf("fwupdate: %lu bytes received, writing to QSPI 0x%x\n", (unsigned long)len,
         (unsigned)QSPI_NOR_FW_OFF);
  if (fw_write(img, len) != 0) {
    printf("fwupdate: FLASH WRITE FAILED: reload an image from U-Boot (serial loady or DFU)\n");
    free(img);
    return 1;
  }
  free(img);
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

void nexus_fwupdate_register(void)
{
  rtems_shell_add_cmd("fwupdate", "misc",
                      "fwupdate [port] [reboot]: receive a FIT image over TCP and flash it",
                      shell_fwupdate);
}
