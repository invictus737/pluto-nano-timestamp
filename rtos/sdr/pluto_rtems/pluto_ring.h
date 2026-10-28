/*
 * Block ring shared by a stream user (reader or writer thread) and the DMA service thread.
 * Blocks go through the slots strictly in order, so the whole state is three free-running
 * counters (wrap-safe uint32_t differences), each written by exactly one side:
 *
 *   RX: submitted (service) >= done (service) >= released (reader)
 *       slot i is: free when i < released + nslots and not yet submitted,
 *                  in the DMA when done <= i < submitted, ready when released <= i < done.
 *   TX: committed (writer) >= submitted (service) >= done (service)
 *       the writer fills slot `committed` while committed - done < nslots.
 *
 * No locks, no allocation. The counters are sequentially consistent: a block's contents are
 * visible before its counter moves, and a side that moves its counter and then reads the other
 * side's counter cannot miss a concurrent update (no lost wake-up between user and service).
 * Platform independent (host tested).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PLUTO_RING_H
#define PLUTO_RING_H

#include <stdatomic.h>
#include <stdint.h>

#define PLUTO_DMAC_MAX_INFLIGHT 4 /* AXI DMAC transfer IDs are 2 bits */

struct pluto_ring {
  uint32_t nslots;
  uint32_t max_inflight;
  /* RX: user side = released; TX: user side = committed */
  _Atomic uint32_t user;
  _Atomic uint32_t submitted;
  _Atomic uint32_t done;
};

void pluto_ring_init(struct pluto_ring *r, uint32_t nslots, uint32_t max_inflight);
void pluto_ring_reset(struct pluto_ring *r);

/* ---- service side ---- */
/* RX: number of transfers that may be submitted now (free slots, DMA queue limit) */
uint32_t pluto_rx_ring_submittable(struct pluto_ring *r);
/* TX: number of committed blocks not yet submitted, limited by the DMA queue */
uint32_t pluto_tx_ring_submittable(struct pluto_ring *r);
/* slot index of the next submission / the oldest transfer in flight */
uint32_t pluto_ring_next_submit_slot(struct pluto_ring *r);
void pluto_ring_mark_submitted(struct pluto_ring *r);
uint32_t pluto_ring_inflight(struct pluto_ring *r);
void pluto_ring_mark_done(struct pluto_ring *r);

/* ---- RX reader ---- */
/* completed blocks not yet released by the reader (the block being read counts until released) */
uint32_t pluto_rx_ring_ready(struct pluto_ring *r);
uint32_t pluto_rx_ring_read_slot(struct pluto_ring *r);
void pluto_rx_ring_release(struct pluto_ring *r);
/* RX: every slot is filled and waiting for the reader (the DMA is starved) */
int pluto_rx_ring_full(struct pluto_ring *r);

/* ---- TX writer ---- */
/* 1 when slot `committed` is free for the writer */
int pluto_tx_ring_can_fill(struct pluto_ring *r);
uint32_t pluto_tx_ring_fill_slot(struct pluto_ring *r);
void pluto_tx_ring_commit(struct pluto_ring *r);
/* TX: blocks committed but not yet completed by the DMA */
uint32_t pluto_tx_ring_pending(struct pluto_ring *r);

#endif
