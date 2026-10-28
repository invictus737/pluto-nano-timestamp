/*
 * Host unit tests for pluto_ring.c: submission limits, wrap-around of the free-running counters,
 * and a two-thread stress test per direction (a fake DMA service thread completing transfers in
 * order against a reader/writer thread) that checks order, no loss and no slot reuse while held.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pluto_ring.h"

static int failures, checks;
#define CHECK(c)                                                                 \
  do {                                                                           \
    checks++;                                                                    \
    if (!(c)) {                                                                  \
      failures++;                                                                \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);      \
    }                                                                            \
  } while (0)

static void test_limits(void)
{
  struct pluto_ring r;
  pluto_ring_init(&r, 8, 3);
  CHECK(r.max_inflight == 3);
  CHECK(pluto_rx_ring_submittable(&r) == 3);
  for (int i = 0; i < 3; ++i) {
    CHECK(pluto_ring_next_submit_slot(&r) == (uint32_t)i);
    pluto_ring_mark_submitted(&r);
  }
  CHECK(pluto_rx_ring_submittable(&r) == 0 && pluto_ring_inflight(&r) == 3);
  pluto_ring_mark_done(&r);
  CHECK(pluto_rx_ring_ready(&r) == 1 && pluto_rx_ring_read_slot(&r) == 0);
  CHECK(pluto_rx_ring_submittable(&r) == 1);
  /* fill everything without the reader releasing: slots run out at nslots */
  for (int k = 0; k < 20; ++k) {
    uint32_t n = pluto_rx_ring_submittable(&r);
    for (uint32_t i = 0; i < n; ++i)
      pluto_ring_mark_submitted(&r);
    if (pluto_ring_inflight(&r))
      pluto_ring_mark_done(&r);
  }
  CHECK(pluto_rx_ring_ready(&r) == 8 && pluto_rx_ring_submittable(&r) == 0);
  CHECK(pluto_rx_ring_full(&r));
  pluto_rx_ring_release(&r);
  CHECK(pluto_rx_ring_submittable(&r) == 1 && !pluto_rx_ring_full(&r));

  /* clamp of the in-flight limit to the DMAC ID space and to the slot count */
  pluto_ring_init(&r, 2, 9);
  CHECK(r.max_inflight == 2);
  pluto_ring_init(&r, 64, 9);
  CHECK(r.max_inflight == PLUTO_DMAC_MAX_INFLIGHT);

  /* TX */
  pluto_ring_init(&r, 4, 3);
  CHECK(pluto_tx_ring_can_fill(&r) && pluto_tx_ring_submittable(&r) == 0);
  for (int i = 0; i < 4; ++i) {
    CHECK(pluto_tx_ring_can_fill(&r) && pluto_tx_ring_fill_slot(&r) == (uint32_t)i);
    pluto_tx_ring_commit(&r);
  }
  CHECK(!pluto_tx_ring_can_fill(&r) && pluto_tx_ring_pending(&r) == 4);
  CHECK(pluto_tx_ring_submittable(&r) == 3);
  for (int i = 0; i < 3; ++i)
    pluto_ring_mark_submitted(&r);
  CHECK(pluto_tx_ring_submittable(&r) == 0);
  pluto_ring_mark_done(&r);
  CHECK(pluto_tx_ring_can_fill(&r) && pluto_tx_ring_fill_slot(&r) == 0);
  CHECK(pluto_tx_ring_submittable(&r) == 1);
}

static void test_wrap(void)
{
  /* counters near 2^32: the differences stay right across the wrap */
  struct pluto_ring r;
  pluto_ring_init(&r, 16, 3);
  uint32_t base = 0xfffffff0u;
  atomic_store(&r.user, base);
  atomic_store(&r.submitted, base);
  atomic_store(&r.done, base);
  uint64_t delivered = 0;
  uint32_t expect_slot = base % 16;
  for (int step = 0; step < 1000; ++step) {
    uint32_t n = pluto_rx_ring_submittable(&r);
    for (uint32_t i = 0; i < n; ++i)
      pluto_ring_mark_submitted(&r);
    if (pluto_ring_inflight(&r))
      pluto_ring_mark_done(&r);
    if (pluto_rx_ring_ready(&r) && (step % 3)) {
      CHECK(pluto_rx_ring_read_slot(&r) == expect_slot);
      expect_slot = (expect_slot + 1) % 16;
      pluto_rx_ring_release(&r);
      delivered++;
    }
    CHECK(pluto_ring_inflight(&r) <= 3);
    CHECK(atomic_load(&r.submitted) - atomic_load(&r.user) <= 16);
  }
  CHECK(delivered > 500);
  CHECK(atomic_load(&r.user) < base); /* really wrapped */
}

/* ---- threaded stress ---- */

#define NSLOTS 8
#define BLOCKS 200000
struct stress {
  struct pluto_ring r;
  _Atomic uint32_t owner[NSLOTS]; /* 0 free, 1 dma, 2 ready/held by user */
  uint64_t payload[NSLOTS];
  _Atomic int stop;
  uint64_t produced, consumed;
  int errors;
};

static void *rx_service(void *arg)
{
  struct stress *s = arg;
  uint64_t seq = 0;
  uint32_t fifo[PLUTO_DMAC_MAX_INFLIGHT], head = 0, count = 0;
  while (!atomic_load(&s->stop)) {
    /* complete the oldest transfer: "DMA" writes the payload */
    if (count) {
      uint32_t slot = fifo[head];
      head = (head + 1) % PLUTO_DMAC_MAX_INFLIGHT;
      count--;
      if (atomic_load(&s->owner[slot]) != 1)
        s->errors++;
      s->payload[slot] = seq++;
      atomic_store(&s->owner[slot], 2);
      pluto_ring_mark_done(&s->r);
    }
    uint32_t n = pluto_rx_ring_submittable(&s->r);
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t slot = pluto_ring_next_submit_slot(&s->r);
      if (atomic_load(&s->owner[slot]) != 0)
        s->errors++;
      atomic_store(&s->owner[slot], 1);
      fifo[(head + count) % PLUTO_DMAC_MAX_INFLIGHT] = slot;
      count++;
      pluto_ring_mark_submitted(&s->r);
    }
    if (seq >= BLOCKS)
      break;
    if ((seq & 63) == 0)
      sched_yield();
  }
  s->produced = seq;
  return NULL;
}

static void test_rx_stress(void)
{
  struct stress *s = calloc(1, sizeof(*s));
  pluto_ring_init(&s->r, NSLOTS, 3);
  pthread_t th;
  pthread_create(&th, NULL, rx_service, s);
  uint64_t expect = 0;
  while (expect < BLOCKS) {
    if (!pluto_rx_ring_ready(&s->r)) {
      sched_yield();
      continue;
    }
    uint32_t slot = pluto_rx_ring_read_slot(&s->r);
    if (atomic_load(&s->owner[slot]) != 2 || s->payload[slot] != expect)
      s->errors++;
    expect++;
    atomic_store(&s->owner[slot], 0);
    pluto_rx_ring_release(&s->r);
  }
  pthread_join(th, NULL);
  CHECK(s->errors == 0);
  CHECK(expect == BLOCKS && s->produced == BLOCKS);
  free(s);
}

static void *tx_service(void *arg)
{
  struct stress *s = arg;
  uint64_t expect = 0;
  uint32_t fifo[PLUTO_DMAC_MAX_INFLIGHT], head = 0, count = 0;
  while (expect < BLOCKS) {
    uint32_t n = pluto_tx_ring_submittable(&s->r);
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t slot = pluto_ring_next_submit_slot(&s->r);
      if (atomic_load(&s->owner[slot]) != 2)
        s->errors++;
      atomic_store(&s->owner[slot], 1);
      fifo[(head + count) % PLUTO_DMAC_MAX_INFLIGHT] = slot;
      count++;
      pluto_ring_mark_submitted(&s->r);
    }
    if (count) {
      uint32_t slot = fifo[head];
      head = (head + 1) % PLUTO_DMAC_MAX_INFLIGHT;
      count--;
      if (s->payload[slot] != expect)
        s->errors++;
      expect++;
      atomic_store(&s->owner[slot], 0);
      pluto_ring_mark_done(&s->r);
    } else {
      sched_yield();
    }
  }
  s->consumed = expect;
  return NULL;
}

static void test_tx_stress(void)
{
  struct stress *s = calloc(1, sizeof(*s));
  pluto_ring_init(&s->r, NSLOTS, 3);
  pthread_t th;
  pthread_create(&th, NULL, tx_service, s);
  uint64_t seq = 0;
  while (seq < BLOCKS) {
    if (!pluto_tx_ring_can_fill(&s->r)) {
      sched_yield();
      continue;
    }
    uint32_t slot = pluto_tx_ring_fill_slot(&s->r);
    if (atomic_load(&s->owner[slot]) != 0)
      s->errors++;
    s->payload[slot] = seq++;
    atomic_store(&s->owner[slot], 2);
    pluto_tx_ring_commit(&s->r);
  }
  pthread_join(th, NULL);
  CHECK(s->errors == 0);
  CHECK(s->consumed == BLOCKS);
  CHECK(pluto_tx_ring_pending(&s->r) == 0);
  free(s);
}

int main(void)
{
  test_limits();
  test_wrap();
  test_rx_stress();
  test_tx_stress();
  printf("test_ring: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
