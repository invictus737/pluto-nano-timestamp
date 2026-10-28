/*
 * AXI DMAC block streaming on RTEMS (see pluto_dma.h).
 *
 * Register map: ADI axi_dmac (hdl library/axi_dmac, regmap v4.x), as used by the Linux
 * dma-axi-dmac driver: TRANSFER_ID read before TRANSFER_SUBMIT gives the ID of the transfer being
 * queued, TRANSFER_SUBMIT bit 0 reads 1 while the request queue is full, TRANSFER_DONE has one bit
 * per ID that is set on completion and cleared when that ID is queued again, EOT interrupt per
 * completed transfer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pluto_dma.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rtems.h>
#include <rtems/irq-extension.h>
#include <rtems/thread.h>

#include "pluto_regs.h"
#include "pluto_ring.h"
#include "pluto_rt.h"

#define SVC_POLL_MS 50 /* safety net: the service thread also wakes this often */

struct pluto_dma {
  enum pluto_dma_dir dir;
  uintptr_t base;
  rtems_vector_number irq;
  uint8_t *pool;
  uint32_t block_bytes;
  uint32_t stride;               /* slot spacing: block_bytes rounded up to 64 (cache lines) */
  struct pluto_ring ring;
  /* transfer IDs in flight, oldest first (service thread only, under lock) */
  uint8_t ids[PLUTO_DMAC_MAX_INFLIGHT];
  uint32_t ids_head, ids_count;
  volatile int running;
  volatile int quit;
  int irq_installed;
  pthread_t thread;
  int thread_ok;
  rtems_mutex lock;              /* controller registers + ids: service thread vs start/stop */
  rtems_binary_semaphore svc;    /* ISR / user -> service thread */
  rtems_binary_semaphore user;   /* service thread -> stream user (data / space available) */
  volatile uint64_t irqs, blocks, starved, underruns;
  uint32_t version;
  int had_data;
  void (*on_block)(void *ctx, const uint8_t *block);
  void *on_block_ctx;
};

static inline uint32_t rd(struct pluto_dma *d, uint32_t off)
{
  return *(volatile uint32_t *)(d->base + off);
}

static inline void wr(struct pluto_dma *d, uint32_t off, uint32_t v)
{
  *(volatile uint32_t *)(d->base + off) = v;
}

static void dmac_isr(void *arg)
{
  struct pluto_dma *d = arg;
  uint32_t pending = rd(d, DMAC_REG_IRQ_PENDING);
  wr(d, DMAC_REG_IRQ_PENDING, pending); /* level interrupt: drop it before leaving */
  d->irqs++;
  rtems_binary_semaphore_post(&d->svc);
}

/* caller holds d->lock */
static int service_locked(struct pluto_dma *d)
{
  int reaped = 0;
  if (!d->running)
    return 0;
  uint32_t done_mask = rd(d, DMAC_REG_TRANSFER_DONE);
  while (d->ids_count && (done_mask & (1u << d->ids[d->ids_head]))) {
    d->ids_head = (d->ids_head + 1) % PLUTO_DMAC_MAX_INFLIGHT;
    d->ids_count--;
    if (d->dir == PLUTO_DMA_RX) {
      /* the oldest in-flight slot; it stays untouched until the reader releases it. The pool is
       * non-cacheable, so this is only insurance against a cached fallback allocation. */
      uint8_t *blk = d->pool + (size_t)(atomic_load(&d->ring.done) % d->ring.nslots) * d->stride;
      rtems_cache_invalidate_multiple_data_lines(blk, d->block_bytes);
      if (d->on_block)
        d->on_block(d->on_block_ctx, blk);
    }
    pluto_ring_mark_done(&d->ring);
    d->blocks++;
    reaped++;
  }
  uint32_t n = d->dir == PLUTO_DMA_RX ? pluto_rx_ring_submittable(&d->ring)
                                      : pluto_tx_ring_submittable(&d->ring);
  while (n-- > 0 && d->ids_count < d->ring.max_inflight) {
    if (rd(d, DMAC_REG_TRANSFER_SUBMIT) & 1u)
      break; /* controller queue full: the next EOT brings us back */
    uint32_t slot = pluto_ring_next_submit_slot(&d->ring);
    uint32_t addr = (uint32_t)(uintptr_t)(d->pool + (size_t)slot * d->stride);
    uint8_t id = (uint8_t)(rd(d, DMAC_REG_TRANSFER_ID) & 3u);
    if (d->dir == PLUTO_DMA_RX) {
      wr(d, DMAC_REG_DEST_ADDRESS, addr);
      wr(d, DMAC_REG_DEST_STRIDE, 0);
      wr(d, DMAC_REG_FLAGS, 0);
    } else {
      /* the CPU wrote the block through Normal memory (store buffers, PL310): make it reach DDR
       * before the DMAC (HP port, outside the CPU caches) reads it */
      rtems_cache_flush_multiple_data_lines(d->pool + (size_t)slot * d->stride, d->block_bytes);
      wr(d, DMAC_REG_SRC_ADDRESS, addr);
      wr(d, DMAC_REG_SRC_STRIDE, 0);
      /* one block = one descriptor ending with TLAST, as the Linux iio DMA buffer does */
      wr(d, DMAC_REG_FLAGS, DMAC_FLAG_LAST);
    }
    wr(d, DMAC_REG_X_LENGTH, d->block_bytes - 1u);
    wr(d, DMAC_REG_Y_LENGTH, 0);
    wr(d, DMAC_REG_TRANSFER_SUBMIT, 1u);
    d->ids[(d->ids_head + d->ids_count) % PLUTO_DMAC_MAX_INFLIGHT] = id;
    d->ids_count++;
    pluto_ring_mark_submitted(&d->ring);
  }
  if (d->dir == PLUTO_DMA_RX) {
    if (pluto_rx_ring_full(&d->ring))
      d->starved++;
  } else {
    if (d->ids_count)
      d->had_data = 1;
    else if (d->had_data) {
      d->had_data = 0;
      d->underruns++;
    }
  }
  return reaped;
}

static void *svc_main(void *arg)
{
  struct pluto_dma *d = arg;
  uint32_t poll = (uint32_t)((rtems_clock_get_ticks_per_second() * SVC_POLL_MS + 999) / 1000);
  if (poll == 0)
    poll = 1;
  while (!d->quit) {
    rtems_binary_semaphore_wait_timed_ticks(&d->svc, poll);
    if (d->quit)
      break;
    rtems_mutex_lock(&d->lock);
    int reaped = service_locked(d);
    rtems_mutex_unlock(&d->lock);
    if (reaped)
      rtems_binary_semaphore_post(&d->user);
  }
  return NULL;
}

struct pluto_dma *pluto_dma_create(enum pluto_dma_dir dir, uint32_t block_bytes, uint32_t nslots)
{
  if (block_bytes == 0 || (block_bytes & 7u) || nslots < 2)
    return NULL;
  struct pluto_dma *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->dir = dir;
  d->base = dir == PLUTO_DMA_RX ? PLUTO_RX_DMAC_BASE : PLUTO_TX_DMAC_BASE;
  d->irq = dir == PLUTO_DMA_RX ? PLUTO_RX_DMAC_IRQ : PLUTO_TX_DMAC_IRQ;
  d->block_bytes = block_bytes;
  d->stride = (block_bytes + 63u) & ~63u;
  pluto_ring_init(&d->ring, nslots, PLUTO_DMA_INFLIGHT);
  rtems_mutex_init(&d->lock, dir == PLUTO_DMA_RX ? "plrx" : "pltx");
  rtems_binary_semaphore_init(&d->svc, "plsvc");
  rtems_binary_semaphore_init(&d->user, "plusr");

  d->version = rd(d, DMAC_REG_VERSION);
  uint32_t ident = rd(d, DMAC_REG_IDENT);
  printf("pluto: %s DMAC 0x%08x v%u.%u.%c%s, %u blocks x %u bytes\n",
         dir == PLUTO_DMA_RX ? "RX" : "TX", (unsigned)d->base, (unsigned)(d->version >> 16),
         (unsigned)((d->version >> 8) & 0xff), (char)(d->version & 0xff),
         ident == DMAC_IDENT_MAGIC ? "" : " (no ident)", (unsigned)nslots, (unsigned)block_bytes);
  if (d->version == 0 || d->version == 0xffffffffu) {
    printf("pluto: DMAC at 0x%08x does not answer (PL not loaded?)\n", (unsigned)d->base);
    goto fail;
  }

  d->pool = rtems_cache_coherent_allocate((size_t)nslots * d->stride, 64, 0);
  if (!d->pool) {
    printf("pluto: no DMA-coherent memory for %u bytes\n", (unsigned)(nslots * d->stride));
    goto fail;
  }
  memset(d->pool, 0, (size_t)nslots * d->stride);
  rtems_cache_flush_multiple_data_lines(d->pool, (size_t)nslots * d->stride);

  wr(d, DMAC_REG_CTRL, 0);
  wr(d, DMAC_REG_IRQ_MASK, DMAC_IRQ_SOT | DMAC_IRQ_EOT);
  wr(d, DMAC_REG_IRQ_PENDING, 0xffffffffu);
  rtems_status_code sc = rtems_interrupt_handler_install(
    d->irq, dir == PLUTO_DMA_RX ? "pluto-rx-dma" : "pluto-tx-dma", RTEMS_INTERRUPT_UNIQUE,
    dmac_isr, d);
  if (sc != RTEMS_SUCCESSFUL) {
    printf("pluto: DMAC IRQ %u: %s\n", (unsigned)d->irq, rtems_status_text(sc));
    goto fail;
  }
  d->irq_installed = 1;
  pluto_rt_irq_affinity(d->irq);

  if (pluto_rt_thread_create(&d->thread, dir == PLUTO_DMA_RX ? "PLRX" : "PLTX", svc_main, d) != 0) {
    printf("pluto: cannot start the DMA service thread\n");
    goto fail;
  }
  d->thread_ok = 1;
  return d;
fail:
  pluto_dma_destroy(d);
  return NULL;
}

void pluto_dma_destroy(struct pluto_dma *d)
{
  if (!d)
    return;
  pluto_dma_stop(d);
  if (d->thread_ok) {
    d->quit = 1;
    rtems_binary_semaphore_post(&d->svc);
    pthread_join(d->thread, NULL);
  }
  if (d->irq_installed)
    rtems_interrupt_handler_remove(d->irq, dmac_isr, d);
  if (d->pool) {
    /* a disabled DMAC finishes its current burst: let it land before the memory is reused */
    rtems_task_wake_after(2);
    rtems_cache_coherent_free(d->pool);
  }
  rtems_binary_semaphore_destroy(&d->svc);
  rtems_binary_semaphore_destroy(&d->user);
  rtems_mutex_destroy(&d->lock);
  free(d);
}

int pluto_dma_start(struct pluto_dma *d)
{
  rtems_mutex_lock(&d->lock);
  wr(d, DMAC_REG_CTRL, 0);
  pluto_ring_reset(&d->ring);
  d->ids_head = d->ids_count = 0;
  d->had_data = 0;
  wr(d, DMAC_REG_IRQ_PENDING, 0xffffffffu);
  wr(d, DMAC_REG_IRQ_MASK, DMAC_IRQ_SOT); /* end-of-transfer only */
  wr(d, DMAC_REG_CTRL, DMAC_CTRL_ENABLE);
  d->running = 1;
  service_locked(d);
  rtems_mutex_unlock(&d->lock);
  return 0;
}

void pluto_dma_stop(struct pluto_dma *d)
{
  rtems_mutex_lock(&d->lock);
  d->running = 0;
  wr(d, DMAC_REG_IRQ_MASK, DMAC_IRQ_SOT | DMAC_IRQ_EOT);
  wr(d, DMAC_REG_CTRL, 0); /* aborts queued transfers */
  wr(d, DMAC_REG_IRQ_PENDING, 0xffffffffu);
  pluto_ring_reset(&d->ring);
  d->ids_head = d->ids_count = 0;
  rtems_mutex_unlock(&d->lock);
  rtems_binary_semaphore_post(&d->user); /* release a waiting user: it sees no data */
}

int pluto_dma_running(struct pluto_dma *d)
{
  return d->running;
}

/* ticks to wait for timeout_us (rounded up), 0 for a non-blocking call */
static uint32_t us_to_ticks(long timeout_us)
{
  if (timeout_us <= 0)
    return 0;
  uint64_t t = ((uint64_t)timeout_us * rtems_clock_get_ticks_per_second() + 999999u) / 1000000u;
  return t ? (uint32_t)t : 1u;
}

/* wait until cond(d) holds or the timeout expires; 0 ok, -1 timeout */
static int wait_for(struct pluto_dma *d, int (*cond)(struct pluto_dma *), long timeout_us)
{
  if (cond(d))
    return 0;
  uint32_t total = us_to_ticks(timeout_us);
  if (total == 0)
    return -1;
  rtems_interval start = rtems_clock_get_ticks_since_boot();
  for (;;) {
    rtems_interval used = rtems_clock_get_ticks_since_boot() - start;
    if (used >= total)
      return cond(d) ? 0 : -1;
    rtems_binary_semaphore_wait_timed_ticks(&d->user, total - used);
    if (cond(d))
      return 0;
    if (!d->running)
      return -1; /* stopped meanwhile */
  }
}

static int rx_ready(struct pluto_dma *d)
{
  return d->running && pluto_rx_ring_ready(&d->ring) > 0;
}

static int tx_space(struct pluto_dma *d)
{
  return d->running && pluto_tx_ring_can_fill(&d->ring);
}

static void kick(struct pluto_dma *d)
{
  /* only needed when the controller queue has room; otherwise the next EOT does it */
  if (pluto_ring_inflight(&d->ring) < d->ring.max_inflight)
    rtems_binary_semaphore_post(&d->svc);
}

int pluto_dma_rx_next(struct pluto_dma *d, const uint8_t **block, long timeout_us)
{
  if (wait_for(d, rx_ready, timeout_us) != 0)
    return -1;
  *block = d->pool + (size_t)pluto_rx_ring_read_slot(&d->ring) * d->stride;
  return 0;
}

void pluto_dma_rx_release(struct pluto_dma *d)
{
  if (!d->running)
    return;
  pluto_rx_ring_release(&d->ring);
  kick(d);
}

int pluto_dma_tx_acquire(struct pluto_dma *d, uint8_t **block, long timeout_us)
{
  if (wait_for(d, tx_space, timeout_us) != 0)
    return -1;
  *block = d->pool + (size_t)pluto_tx_ring_fill_slot(&d->ring) * d->stride;
  return 0;
}

void pluto_dma_tx_commit(struct pluto_dma *d)
{
  if (!d->running)
    return;
  pluto_tx_ring_commit(&d->ring);
  kick(d);
}

uint32_t pluto_dma_tx_pending(struct pluto_dma *d)
{
  return pluto_tx_ring_pending(&d->ring);
}

void pluto_dma_set_block_hook(struct pluto_dma *d, void (*fn)(void *ctx, const uint8_t *block),
                              void *ctx)
{
  rtems_mutex_lock(&d->lock);
  d->on_block_ctx = ctx;
  d->on_block = fn;
  rtems_mutex_unlock(&d->lock);
}

void pluto_dma_get_stats(struct pluto_dma *d, struct pluto_dma_stats *s)
{
  s->irqs = d->irqs;
  s->blocks = d->blocks;
  s->starved = d->starved;
  s->underruns = d->underruns;
  s->inflight = pluto_ring_inflight(&d->ring);
  s->version = d->version;
}
