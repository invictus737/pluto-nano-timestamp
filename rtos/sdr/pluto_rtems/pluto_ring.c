/*
 * Lock-free block ring (see pluto_ring.h). Platform independent.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pluto_ring.h"

#define LD(x) atomic_load_explicit(&(x), memory_order_seq_cst)
#define ST(x, v) atomic_store_explicit(&(x), (v), memory_order_seq_cst)

void pluto_ring_init(struct pluto_ring *r, uint32_t nslots, uint32_t max_inflight)
{
  r->nslots = nslots;
  if (max_inflight == 0 || max_inflight > PLUTO_DMAC_MAX_INFLIGHT)
    max_inflight = PLUTO_DMAC_MAX_INFLIGHT;
  if (max_inflight > nslots)
    max_inflight = nslots;
  r->max_inflight = max_inflight;
  pluto_ring_reset(r);
}

void pluto_ring_reset(struct pluto_ring *r)
{
  ST(r->user, 0);
  ST(r->submitted, 0);
  ST(r->done, 0);
}

uint32_t pluto_ring_inflight(struct pluto_ring *r)
{
  return LD(r->submitted) - LD(r->done);
}

uint32_t pluto_rx_ring_submittable(struct pluto_ring *r)
{
  uint32_t sub = LD(r->submitted), done = LD(r->done), rel = LD(r->user);
  uint32_t free_slots = r->nslots - (sub - rel);
  uint32_t queue = r->max_inflight - (sub - done);
  return free_slots < queue ? free_slots : queue;
}

uint32_t pluto_tx_ring_submittable(struct pluto_ring *r)
{
  uint32_t com = LD(r->user), sub = LD(r->submitted), done = LD(r->done);
  uint32_t waiting = com - sub;
  uint32_t queue = r->max_inflight - (sub - done);
  return waiting < queue ? waiting : queue;
}

uint32_t pluto_ring_next_submit_slot(struct pluto_ring *r)
{
  return LD(r->submitted) % r->nslots;
}

void pluto_ring_mark_submitted(struct pluto_ring *r)
{
  ST(r->submitted, LD(r->submitted) + 1);
}

void pluto_ring_mark_done(struct pluto_ring *r)
{
  ST(r->done, LD(r->done) + 1);
}

uint32_t pluto_rx_ring_ready(struct pluto_ring *r)
{
  return LD(r->done) - LD(r->user);
}

uint32_t pluto_rx_ring_read_slot(struct pluto_ring *r)
{
  return LD(r->user) % r->nslots;
}

void pluto_rx_ring_release(struct pluto_ring *r)
{
  ST(r->user, LD(r->user) + 1);
}

int pluto_rx_ring_full(struct pluto_ring *r)
{
  return LD(r->submitted) - LD(r->user) == r->nslots && pluto_ring_inflight(r) == 0;
}

int pluto_tx_ring_can_fill(struct pluto_ring *r)
{
  return LD(r->user) - LD(r->done) < r->nslots;
}

uint32_t pluto_tx_ring_fill_slot(struct pluto_ring *r)
{
  return LD(r->user) % r->nslots;
}

void pluto_tx_ring_commit(struct pluto_ring *r)
{
  ST(r->user, LD(r->user) + 1);
}

uint32_t pluto_tx_ring_pending(struct pluto_ring *r)
{
  return LD(r->user) - LD(r->done);
}
