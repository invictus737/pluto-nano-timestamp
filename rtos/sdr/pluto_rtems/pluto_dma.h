/*
 * AXI DMAC block streaming for the Pluto RX (dev->mem) and TX (mem->dev) paths on RTEMS.
 *
 * One fixed pool of equally sized blocks (DMA-coherent memory, allocated once) per direction,
 * used as a ring (pluto_ring.h). Every block is one DMAC transfer. The DMAC end-of-transfer
 * interrupt acknowledges the controller and wakes the direction's service thread, which reaps
 * completed transfers in order (TRANSFER_DONE bitmap against the queued transfer IDs), queues
 * new ones (at most PLUTO_DMA_INFLIGHT in the controller) and wakes the stream user. No busy
 * waiting and no allocation after pluto_dma_create().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PLUTO_DMA_H
#define PLUTO_DMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLUTO_DMA_INFLIGHT 3

enum pluto_dma_dir { PLUTO_DMA_RX, PLUTO_DMA_TX };

struct pluto_dma;

struct pluto_dma_stats {
  uint64_t irqs;      /* DMAC interrupts */
  uint64_t blocks;    /* transfers completed */
  uint64_t starved;   /* RX: service found every slot full (reader behind, data will be lost) */
  uint64_t underruns; /* TX: the DMA ran dry while running (nothing queued) */
  uint32_t inflight;
  uint32_t version;   /* DMAC VERSION register */
};

/* NULL on failure (logged). block_bytes must be a multiple of 8. Starts the service thread. */
struct pluto_dma *pluto_dma_create(enum pluto_dma_dir dir, uint32_t block_bytes, uint32_t nslots);
void pluto_dma_destroy(struct pluto_dma *d);

/* start: reset the ring, enable the controller and (RX) queue the first transfers */
int pluto_dma_start(struct pluto_dma *d);
/* stop: disable the controller (queued transfers are aborted) and reset the ring */
void pluto_dma_stop(struct pluto_dma *d);
int pluto_dma_running(struct pluto_dma *d);

/* RX user: next completed block (0) or -1 on timeout; release it when done */
int pluto_dma_rx_next(struct pluto_dma *d, const uint8_t **block, long timeout_us);
void pluto_dma_rx_release(struct pluto_dma *d);

/* TX user: a free block to fill (0) or -1 on timeout; commit hands it to the DMA */
int pluto_dma_tx_acquire(struct pluto_dma *d, uint8_t **block, long timeout_us);
void pluto_dma_tx_commit(struct pluto_dma *d);
/* TX: blocks committed and not yet sent */
uint32_t pluto_dma_tx_pending(struct pluto_dma *d);

/* called by the service thread for every completed block, in order, before the user sees it
 * (RX: the time anchor of the hardware clock reads the block's stamp here) */
void pluto_dma_set_block_hook(struct pluto_dma *d, void (*fn)(void *ctx, const uint8_t *block),
                              void *ctx);

void pluto_dma_get_stats(struct pluto_dma *d, struct pluto_dma_stats *s);

#ifdef __cplusplus
}
#endif
#endif
