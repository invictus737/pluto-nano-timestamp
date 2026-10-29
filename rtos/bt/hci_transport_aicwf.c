/*
 * BTstack HCI transport over aicwf_bt_* (see hci_transport_aicwf.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hci_transport_aicwf.h"

#include "aicwf_bt.h"
#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "hci.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SLOT_BYTES (4 + HCI_ACL_PAYLOAD_SIZE) /* ACL header + payload; events need 2 + 255 */
#define NSLOTS 16                             /* power of two */
#define READ_CHUNK 2048
#define READ_TIMEOUT_MS 100

typedef struct {
  uint8_t type;
  uint16_t len;
  uint8_t data[SLOT_BYTES];
} rx_slot_t;

/* single producer (reader thread) / single consumer (run loop) */
static rx_slot_t slots[NSLOTS];
static atomic_uint slot_w, slot_r;
static pthread_mutex_t slot_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t slot_cond = PTHREAD_COND_INITIALIZER; /* consumer freed a slot */

static void (*packet_handler)(uint8_t type, uint8_t *packet, uint16_t size);
static btstack_data_source_t data_source;
static pthread_t reader;
static atomic_bool reader_run;
static bool reader_started;
static int reader_prio = 130, reader_cpu = 0;

static atomic_uint st_rx, st_drop, st_resync, st_tx, st_txerr;
static atomic_int st_failed;

/* ---- receive: H:4 stream -> packets ---- */

typedef struct {
  rx_slot_t *slot; /* NULL: waiting for a type byte */
  uint16_t have;   /* bytes in slot->data */
  uint16_t need;   /* total expected (header first, then header + payload) */
  uint16_t hdr;
  uint32_t skip; /* bytes of an oversized packet still to discard */
  bool sized;
} parser_t;

static rx_slot_t *slot_acquire(void)
{
  for (;;) {
    unsigned w = atomic_load_explicit(&slot_w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&slot_r, memory_order_acquire);
    if (w - r < NSLOTS) {
      return &slots[w & (NSLOTS - 1)];
    }
    if (!atomic_load(&reader_run)) {
      return NULL;
    }
    /* run loop behind: wait (the controller's own ring keeps buffering meanwhile) */
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += 20 * 1000000L;
    if (dl.tv_nsec >= 1000000000L) {
      dl.tv_sec++;
      dl.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&slot_lock);
    if (atomic_load(&slot_w) - atomic_load(&slot_r) >= NSLOTS) {
      (void)pthread_cond_timedwait(&slot_cond, &slot_lock, &dl);
    }
    pthread_mutex_unlock(&slot_lock);
  }
}

static void slot_commit(void)
{
  atomic_fetch_add_explicit(&slot_w, 1, memory_order_release);
  atomic_fetch_add(&st_rx, 1);
  btstack_run_loop_poll_data_sources_from_irq();
}

static uint16_t header_len(uint8_t type)
{
  switch (type) {
  case HCI_EVENT_PACKET:
    return 2;
  case HCI_ACL_DATA_PACKET:
    return 4;
  case HCI_SCO_DATA_PACKET:
    return 3;
  case HCI_ISO_DATA_PACKET:
    return 4;
  default:
    return 0;
  }
}

static uint16_t payload_len(uint8_t type, const uint8_t *h)
{
  switch (type) {
  case HCI_EVENT_PACKET:
    return h[1];
  case HCI_ACL_DATA_PACKET:
    return little_endian_read_16(h, 2);
  case HCI_SCO_DATA_PACKET:
    return h[2];
  default: /* ISO */
    return little_endian_read_16(h, 2) & 0x3fff;
  }
}

static void parse(parser_t *p, const uint8_t *buf, size_t n)
{
  size_t i = 0;
  while (i < n) {
    if (p->skip) {
      size_t k = n - i < p->skip ? n - i : p->skip;
      p->skip -= (uint32_t)k;
      i += k;
      continue;
    }
    if (p->slot == NULL) {
      uint8_t type = buf[i++];
      uint16_t hl = header_len(type);
      if (hl == 0) {
        atomic_fetch_add(&st_resync, 1); /* not a packet start: resynchronise byte by byte */
        continue;
      }
      p->slot = slot_acquire();
      if (p->slot == NULL) {
        return; /* closing */
      }
      p->slot->type = type;
      p->have = 0;
      p->hdr = hl;
      p->need = hl;
      p->sized = false;
      continue;
    }
    size_t k = p->need - p->have;
    if (k > n - i) {
      k = n - i;
    }
    memcpy(p->slot->data + p->have, buf + i, k);
    p->have += (uint16_t)k;
    i += k;
    if (p->have < p->need) {
      continue;
    }
    if (!p->sized) {
      uint32_t total = (uint32_t)p->hdr + payload_len(p->slot->type, p->slot->data);
      p->sized = true;
      if (total > SLOT_BYTES) {
        atomic_fetch_add(&st_drop, 1);
        p->skip = total - p->have;
        p->slot = NULL;
        continue;
      }
      p->need = (uint16_t)total;
      if (p->have < p->need) {
        continue;
      }
    }
    p->slot->len = p->need;
    p->slot = NULL;
    slot_commit();
  }
}

static void *reader_main(void *arg)
{
  (void)arg;
  static uint8_t chunk[READ_CHUNK];
  parser_t p;
  memset(&p, 0, sizeof(p));
  while (atomic_load(&reader_run)) {
    int n = aicwf_bt_read(chunk, sizeof(chunk), READ_TIMEOUT_MS);
    if (n > 0) {
      parse(&p, chunk, (size_t)n);
    } else if (n < 0) {
      struct timespec ts = {0, 50 * 1000000L};
      nanosleep(&ts, NULL);
    }
  }
  return NULL;
}

/* run loop thread: deliver every complete packet */
static void deliver(btstack_data_source_t *ds, btstack_data_source_callback_type_t type)
{
  (void)ds;
  (void)type;
  for (;;) {
    unsigned r = atomic_load_explicit(&slot_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&slot_w, memory_order_acquire);
    if (r == w) {
      return;
    }
    rx_slot_t *s = &slots[r & (NSLOTS - 1)];
    if (packet_handler != NULL) {
      packet_handler(s->type, s->data, s->len);
    }
    atomic_store_explicit(&slot_r, r + 1, memory_order_release);
    if (w - r >= NSLOTS) {
      pthread_mutex_lock(&slot_lock);
      pthread_cond_signal(&slot_cond);
      pthread_mutex_unlock(&slot_lock);
    }
  }
}

/* ---- hci_transport_t ---- */

static void tr_init(const void *config)
{
  (void)config;
}

static int tr_open(void)
{
  int rc = aicwf_bt_open();
  if (rc != 0) {
    log_error("aicwf: no Bluetooth controller (%d)", rc); /* quiet while the manager retries */
    return -1;
  }
  atomic_store(&slot_w, 0);
  atomic_store(&slot_r, 0);
  atomic_store(&st_failed, 0);
  atomic_store(&reader_run, true);

  pthread_attr_t at;
  struct sched_param sp = {.sched_priority = reader_prio};
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 16 * 1024);
#ifndef __APPLE__
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&at, SCHED_FIFO);
  pthread_attr_setschedparam(&at, &sp);
#else
  (void)sp;
#endif
#ifdef __rtems__
  if (reader_cpu >= 0) {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(reader_cpu, &cpus);
    pthread_attr_setaffinity_np(&at, sizeof(cpus), &cpus);
  }
#endif
  rc = pthread_create(&reader, &at, reader_main, NULL);
  pthread_attr_destroy(&at);
  if (rc != 0) {
    atomic_store(&reader_run, false);
    aicwf_bt_close();
    log_error("aicwf: reader thread not started (%d)", rc);
    return -1;
  }
  reader_started = true;

  data_source.process = deliver;
  btstack_run_loop_set_data_source_fd(&data_source, -1);
  btstack_run_loop_enable_data_source_callbacks(&data_source, DATA_SOURCE_CALLBACK_POLL);
  btstack_run_loop_add_data_source(&data_source);
  return 0;
}

static int tr_close(void)
{
  if (reader_started) {
    atomic_store(&reader_run, false);
    pthread_mutex_lock(&slot_lock);
    pthread_cond_signal(&slot_cond);
    pthread_mutex_unlock(&slot_lock);
    pthread_join(reader, NULL); /* returns within READ_TIMEOUT_MS */
    reader_started = false;
    btstack_run_loop_remove_data_source(&data_source);
  }
  aicwf_bt_close();
  atomic_store(&slot_w, 0);
  atomic_store(&slot_r, 0);
  return 0;
}

static void tr_register_packet_handler(void (*handler)(uint8_t type, uint8_t *packet, uint16_t size))
{
  packet_handler = handler;
}

static int tr_send_packet(uint8_t type, uint8_t *packet, int size)
{
  if (size <= 0) {
    return -1;
  }
  int rc = aicwf_bt_send(type, packet, (size_t)size);
  if (rc != 0) {
    atomic_fetch_add(&st_txerr, 1);
    atomic_store(&st_failed, 1);
    log_error("aicwf: send type %u len %d failed (%d)", type, size, rc);
    return -1;
  }
  atomic_fetch_add(&st_tx, 1);
  return 0;
}

static const hci_transport_t transport = {
    .name = "AICWF",
    .init = tr_init,
    .open = tr_open,
    .close = tr_close,
    .register_packet_handler = tr_register_packet_handler,
    .can_send_packet_now = NULL, /* synchronous */
    .send_packet = tr_send_packet,
    .set_baudrate = NULL,
    .reset_link = NULL,
    .set_sco_config = NULL,
};

const hci_transport_t *hci_transport_aicwf_instance(void)
{
  return &transport;
}

void hci_transport_aicwf_set_thread(int priority, int cpu)
{
  reader_prio = priority;
  reader_cpu = cpu;
}

void hci_transport_aicwf_get_stats(struct hci_transport_aicwf_stats *st)
{
  st->rx_packets = atomic_load(&st_rx);
  st->rx_dropped = atomic_load(&st_drop);
  st->rx_resync = atomic_load(&st_resync);
  st->tx_packets = atomic_load(&st_tx);
  st->tx_errors = atomic_load(&st_txerr);
  st->failed = atomic_load(&st_failed);
}
