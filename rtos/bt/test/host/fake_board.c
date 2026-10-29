/*
 * Host stand-ins for the board side of rtos/bt: the configuration store, the shell registry,
 * the AIC Wi-Fi status, and a scripted HCI controller behind aicwf_bt_* that answers the
 * BTstack init sequence, an inquiry (one audio device without EIR name) and a remote name
 * request. Used by rtos/bt/test/host_link.sh to run the manager end to end on the host.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "aicwf_bt.h"
#include "aicwf_wifi.h"
#include "config_store.h"
#include "rtems/shell.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

char host_data_dir[256];

/* ---- config ---- */
static struct {
  char k[32], v[32];
} kv[16];
static int kvn;
const char *nexus_config_get(const char *key)
{
  for (int i = 0; i < kvn; ++i) {
    if (strcmp(kv[i].k, key) == 0) {
      return kv[i].v;
    }
  }
  return NULL;
}
int nexus_config_set(const char *key, const char *value)
{
  for (int i = 0; i < kvn; ++i) {
    if (strcmp(kv[i].k, key) == 0) {
      snprintf(kv[i].v, sizeof(kv[i].v), "%s", value ? value : "");
      return 0;
    }
  }
  snprintf(kv[kvn].k, sizeof(kv[kvn].k), "%s", key);
  snprintf(kv[kvn].v, sizeof(kv[kvn].v), "%s", value ? value : "");
  kvn++;
  return 0;
}

/* ---- shell ---- */
rtems_shell_command_t host_bt_cmd;
void *rtems_shell_add_cmd(const char *cmd, const char *topic, const char *usage, rtems_shell_command_t command)
{
  (void)topic;
  (void)usage;
  if (strcmp(cmd, "bt") == 0) {
    host_bt_cmd = command;
  }
  return NULL;
}

/* ---- Wi-Fi: associated on 2.4 GHz channel 6 ---- */
int aicwf_wifi_status(struct aicwf_wifi_status *st)
{
  memset(st, 0, sizeof(*st));
  st->connected = 1;
  st->channel = 6;
  return 0;
}

/* ---- scripted controller ---- */
int host_controller_present = 1;
unsigned host_afh_commands;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint8_t q[16384];
static size_t qn;
static int opened;

static void push_evt(uint8_t code, const uint8_t *p, uint8_t len)
{
  pthread_mutex_lock(&lk);
  if (qn + 3 + len <= sizeof(q)) {
    q[qn++] = AICWF_BT_H4_EVT;
    q[qn++] = code;
    q[qn++] = len;
    memcpy(q + qn, p, len);
    qn += len;
  }
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&lk);
}

static void cmd_complete(uint16_t op, const uint8_t *ret, uint8_t len)
{
  uint8_t p[255];
  p[0] = 1;
  p[1] = op & 0xff;
  p[2] = op >> 8;
  memcpy(p + 3, ret, len);
  push_evt(0x0e, p, (uint8_t)(3 + len));
}

static void cmd_status(uint16_t op)
{
  uint8_t p[4] = {0, 1, (uint8_t)(op & 0xff), (uint8_t)(op >> 8)};
  push_evt(0x0f, p, 4);
}

static const uint8_t dev_addr[6] = {0x75, 0x17, 0x89, 0x41, 0xe4, 0x0c}; /* little endian */

int host_bt_presence = -1; /* -1: follow host_controller_present */
int aicwf_bt_present(void)
{
  if (host_bt_presence >= 0) {
    return host_bt_presence;
  }
  return host_controller_present ? AICWF_BT_READY : AICWF_BT_NO_DEVICE;
}

int aicwf_bt_stats(struct aicwf_bt_stats *st)
{
  memset(st, 0, sizeof(*st));
  st->vid = 0xa69c;
  st->pid = 0x8d81;
  return 0;
}

int aicwf_bt_open(void)
{
  if (!host_controller_present) {
    return -5;
  }
  opened = 1;
  return 0;
}
int aicwf_bt_close(void)
{
  opened = 0;
  pthread_mutex_lock(&lk);
  qn = 0;
  pthread_mutex_unlock(&lk);
  return 0;
}

int aicwf_bt_send(uint8_t type, const uint8_t *d, size_t len)
{
  if (!opened) {
    return -5;
  }
  if (type != AICWF_BT_H4_CMD || len < 3) {
    return 0;
  }
  uint16_t op = (uint16_t)(d[0] | d[1] << 8);
  uint8_t r[80];
  memset(r, 0, sizeof(r));
  switch (op) {
  case 0x1001: /* local version: 5.0, manufacturer 0x0a69 */
    r[1] = 9;
    r[4] = 9;
    r[5] = 0x69;
    r[6] = 0x0a;
    cmd_complete(op, r, 9);
    break;
  case 0x1002: /* supported commands: all */
    memset(r + 1, 0xff, 64);
    cmd_complete(op, r, 65);
    break;
  case 0x1003: /* features: EDR, SSP, inquiry with RSSI, EIR */
    r[1] = 0xff;
    r[2] = 0xff;
    r[3] = 0x8f;
    r[4] = 0xfe;
    r[5] = 0xdb;
    r[6] = 0xff;
    r[7] = 0x5b;
    r[8] = 0x87;
    cmd_complete(op, r, 9);
    break;
  case 0x1005: /* buffer size: ACL 1021 x 8 */
    r[1] = 0xfd;
    r[2] = 0x03;
    r[4] = 8;
    cmd_complete(op, r, 8);
    break;
  case 0x1009: { /* BD_ADDR */
    static const uint8_t a[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(r + 1, a, 6);
    cmd_complete(op, r, 7);
    break;
  }
  case 0x0c3f: /* AFH host channel classification */
    host_afh_commands++;
    cmd_complete(op, r, 1);
    break;
  case 0x0401: { /* inquiry: one result with RSSI, then complete */
    cmd_status(op);
    uint8_t p[15];
    p[0] = 1;
    memcpy(p + 1, dev_addr, 6);
    p[7] = 1;    /* page scan repetition mode */
    p[8] = 0;    /* reserved */
    p[9] = 0x04; /* class 0x240404: audio headset */
    p[10] = 0x04;
    p[11] = 0x24;
    p[12] = 0x34;
    p[13] = 0x12;
    p[14] = (uint8_t)-52;
    push_evt(0x22, p, 15);
    uint8_t c = 0;
    push_evt(0x01, &c, 1);
    break;
  }
  case 0x0419: { /* remote name request */
    cmd_status(op);
    uint8_t p[255];
    memset(p, 0, sizeof(p));
    p[0] = 0;
    memcpy(p + 1, d + 3, 6);
    strcpy((char *)p + 7, "Fake Speaker");
    push_evt(0x07, p, 255);
    break;
  }
  default:
    cmd_complete(op, r, 64); /* status 0 and zero parameters */
    break;
  }
  return 0;
}

int aicwf_bt_read(uint8_t *buf, size_t len, int timeout_ms)
{
  struct timespec dl;
  clock_gettime(CLOCK_REALTIME, &dl);
  dl.tv_nsec += (long)timeout_ms * 1000000L;
  while (dl.tv_nsec >= 1000000000L) {
    dl.tv_sec++;
    dl.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&lk);
  while (qn == 0) {
    if (pthread_cond_timedwait(&cv, &lk, &dl) != 0) {
      break;
    }
  }
  /* hand out odd-sized pieces to exercise the H:4 reassembly */
  size_t n = qn < len ? qn : len;
  if (n > 7) {
    n = n / 2 + 1;
  }
  memcpy(buf, q, n);
  memmove(q, q + n, qn - n);
  qn -= n;
  pthread_mutex_unlock(&lk);
  return (int)n;
}
