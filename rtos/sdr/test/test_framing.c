/*
 * Host unit tests for pluto_framing.c: block geometry, gpio register values, stamp byte order,
 * tick/ns conversion, the RX reader against a model of util_cpack2_timestamp + AXI DMAC, and the
 * TX writer against a model of util_upack2_timestamp (hold until the stamp, discard late/early).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pluto_framing.h"

static int failures, checks;
#define CHECK(c)                                                                 \
  do {                                                                           \
    checks++;                                                                    \
    if (!(c)) {                                                                  \
      failures++;                                                                \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);      \
    }                                                                            \
  } while (0)

/* ---------- PL RX model: cpack2_timestamp output as DMA blocks ---------- */

/* IQ value of the sample with counter n (distinct, 12-bit sign extended range) */
static int16_t i_of(uint64_t n) { return (int16_t)((int)(n * 7u % 4095u) - 2047); }
static int16_t q_of(uint64_t n) { return (int16_t)(2047 - (int)(n * 13u % 4095u)); }

/* one DMA block as the hardware writes it: [stamp word][E words = every samples] */
static void pl_rx_block(uint8_t *b, uint32_t every, uint64_t stamp)
{
  pluto_stamp_put(b, stamp);
  for (uint32_t k = 0; k < every; ++k) {
    uint8_t *p = b + 8 + 4 * k;
    int16_t i = i_of(stamp + k), q = q_of(stamp + k);
    p[0] = (uint8_t)i; p[1] = (uint8_t)((uint16_t)i >> 8);
    p[2] = (uint8_t)q; p[3] = (uint8_t)((uint16_t)q >> 8);
  }
}

struct fake_rx {
  uint8_t *blocks;
  uint32_t block_bytes, count, next, held, released;
};
static int fake_next(void *ctx, const uint8_t **b, long timeout_us)
{
  (void)timeout_us;
  struct fake_rx *f = ctx;
  if (f->next >= f->count)
    return -1;
  *b = f->blocks + (size_t)f->next * f->block_bytes;
  f->next++;
  f->held++;
  return 0;
}
static void fake_release(void *ctx)
{
  struct fake_rx *f = ctx;
  f->held--;
  f->released++;
}

static void test_geometry(void)
{
  struct pluto_geom g;
  CHECK(pluto_geom_init(&g, 200, 0) == 0);
  CHECK(g.every == 200 && g.data_samples == 200 && g.block_samples == 202);
  CHECK(g.block_bytes == 808 && g.stamp_bytes == 8 && g.gpio_words == 100);
  CHECK(g.block_bytes % 8 == 0);
  CHECK(pluto_geom_init(&g, 1500, 0) == 0 && g.block_samples == 1502 && g.gpio_words == 750);
  CHECK(pluto_geom_init(&g, 201, 0) == -1); /* odd: half a bus word */
  CHECK(pluto_geom_init(&g, 0, 4096) == 0 && g.every == 0 && g.block_samples == 4096 && g.stamp_bytes == 0);
  CHECK(pluto_geom_init(&g, 0, 0) == -1);
  CHECK(pluto_geom_init(&g, PLUTO_MAX_EVERY + 2, 0) == -1);
  /* gpio_out: [31:1] words, [0] FIR enable kept */
  CHECK(pluto_gpio_with_every(0x1, 100) == 0xc9);
  CHECK(pluto_gpio_with_every(0xffffffff, 0) == 0x1);
  CHECK(pluto_gpio_with_every(0x0, 750) == 1500);
  CHECK(pluto_gpio_with_fir(0xc8, 1) == 0xc9 && pluto_gpio_with_fir(0xc9, 0) == 0xc8);
  /* SoapyPlutoSDR update_device_timestamp_every for one channel: value /= 2, << 1 */
  CHECK(pluto_gpio_with_every(0, 1500 / 2) == ((1500u / 2u) << 1));
}

static void test_stamp_bytes(void)
{
  uint8_t b[8];
  pluto_stamp_put(b, 0x0102030405060708ull);
  CHECK(b[0] == 0x08 && b[7] == 0x01); /* little endian like the 64-bit bus word */
  CHECK(pluto_stamp_get(b) == 0x0102030405060708ull);
  uint64_t v = 0xfedcba9876543210ull;
  memcpy(b, &v, 8); /* the gadget reads *(uint64_t *)buffer on a little endian CPU */
  CHECK(pluto_stamp_get(b) == v);
}

static void test_time(void)
{
  const double rates[] = {200000.0, 600000.0, 1000000.0, 1600000.0, 599999.0, 65105.0, 3.84e6};
  for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
    double rate = rates[r];
    for (long long t = 0; t < 50000000LL; t = t * 3 + 7) {
      long long ns = pluto_ticks_to_ns(t, rate);
      CHECK(pluto_ns_to_ticks(ns, rate) == t); /* the round trip is exact */
    }
    /* 2^40 ticks (days of samples) still round trip */
    long long big = 1LL << 40;
    CHECK(pluto_ns_to_ticks(pluto_ticks_to_ns(big, rate), rate) == big);
  }
  CHECK(pluto_ticks_to_ns(200000, 200000.0) == 1000000000LL);
  CHECK(pluto_ticks_to_ns(1, 200000.0) == 5000LL);
  CHECK(pluto_ticks_to_ns(3, 600000.0) == 5000LL);
  CHECK(pluto_ns_to_ticks(1000000000LL, 1e6) == 1000000LL);
}

static void test_rx_reader(void)
{
  const uint32_t every = 200;
  struct pluto_geom g;
  pluto_geom_init(&g, every, 0);
  const uint32_t nblocks = 12;
  struct fake_rx f = {0};
  f.block_bytes = g.block_bytes;
  f.count = nblocks;
  f.blocks = calloc(nblocks, g.block_bytes);
  /* blocks 0..5 contiguous from 1000, then 3 blocks lost (DMA starved), then contiguous */
  uint64_t stamp = 1000;
  for (uint32_t b = 0; b < nblocks; ++b) {
    if (b == 6)
      stamp += 3 * every;
    pl_rx_block(f.blocks + (size_t)b * g.block_bytes, every, stamp);
    stamp += every;
  }
  struct pluto_rx_source src = {&f, fake_next, fake_release};
  struct pluto_rx_reader r;
  pluto_rx_reader_init(&r, &g);
  int16_t buf[2 * 512];
  uint64_t expect = 1000, got_samples = 0;
  const size_t chunks[] = {1, 7, 64, 128, 199, 200, 333, 512};
  unsigned ci = 0;
  int bad_values = 0;
  for (;;) {
    int has_time = 0;
    uint64_t s = 0;
    size_t want = chunks[ci++ % (sizeof(chunks) / sizeof(chunks[0]))];
    int n = pluto_rx_read(&r, &src, PLUTO_FMT_CS16, buf, want, &has_time, &s, 1000);
    if (n == PLUTO_ERR_TIMEOUT)
      break;
    CHECK(n > 0 && (size_t)n <= want && (uint32_t)n <= every);
    CHECK(has_time);
    if (s != expect) {
      /* only the overflow jump: from 2200 to 2200 + 600 */
      CHECK(expect == 1000 + 6 * every && s == expect + 3 * every);
      expect = s;
    }
    for (int k = 0; k < n; ++k)
      if (buf[2 * k] != i_of(s + k) || buf[2 * k + 1] != q_of(s + k))
        bad_values++;
    expect += n;
    got_samples += n;
  }
  CHECK(bad_values == 0);
  CHECK(got_samples == (uint64_t)nblocks * every);
  CHECK(r.track.overflows == 1 && r.track.lost_samples == 3 * every);
  CHECK(r.track.blocks == nblocks && r.track.misframed == 0);
  CHECK(f.held == 0 && f.released == nblocks); /* every block released exactly once */

  /* format conversions on the same data */
  pluto_rx_reader_init(&r, &g);
  f.next = f.held = f.released = 0;
  float cf[2 * 4];
  int8_t c8[2 * 4];
  int has_time;
  uint64_t s;
  CHECK(pluto_rx_read(&r, &src, PLUTO_FMT_CF32, cf, 4, &has_time, &s, 0) == 4);
  for (int k = 0; k < 4; ++k) {
    CHECK(cf[2 * k] == (float)i_of(s + k) / 2048.0f);
    CHECK(cf[2 * k + 1] == (float)q_of(s + k) / 2048.0f);
  }
  CHECK(pluto_rx_read(&r, &src, PLUTO_FMT_CS8, c8, 4, &has_time, &s, 0) == 4);
  for (int k = 0; k < 4; ++k)
    CHECK(c8[2 * k] == (int8_t)(i_of(s + k) >> 4));

  /* a transfer that did not start on a stamp word is rejected (framing error), then the next
   * good block restarts the timeline */
  pluto_rx_reader_init(&r, &g);
  f.next = f.held = f.released = 0;
  pl_rx_block(f.blocks, every, 5000);
  pl_rx_block(f.blocks + g.block_bytes, every, 5200);
  /* block 2: a misaligned transfer, the "stamp" is IQ data (bit 63 set: earlier than expected) */
  memset(f.blocks + 2 * g.block_bytes, 0xa5, g.block_bytes);
  pl_rx_block(f.blocks + 3 * g.block_bytes, every, 9000);
  f.count = 4;
  CHECK(pluto_rx_read(&r, &src, PLUTO_FMT_CS16, buf, 512, &has_time, &s, 0) == 200 && s == 5000);
  CHECK(pluto_rx_read(&r, &src, PLUTO_FMT_CS16, buf, 512, &has_time, &s, 0) == 200 && s == 5200);
  CHECK(pluto_rx_read(&r, &src, PLUTO_FMT_CS16, buf, 512, &has_time, &s, 0) == PLUTO_ERR_FRAMING);
  CHECK(r.track.misframed == 1);
  CHECK(pluto_rx_read(&r, &src, PLUTO_FMT_CS16, buf, 512, &has_time, &s, 0) == 200 && s == 9000);
  CHECK(r.track.overflows == 0); /* a restart is not counted as an overflow */
  CHECK(f.held == 0);

  /* a backwards stamp is framing loss too */
  struct pluto_rx_track t = {0};
  CHECK(pluto_rx_track_block(&t, 1000, 200) == PLUTO_RX_FIRST);
  CHECK(pluto_rx_track_block(&t, 1200, 200) == PLUTO_RX_OK);
  CHECK(pluto_rx_track_block(&t, 1000, 200) == PLUTO_RX_BAD);
  CHECK(pluto_rx_track_block(&t, 1400, 200) == PLUTO_RX_FIRST);
  CHECK(pluto_rx_track_block(&t, 1600 + PLUTO_RX_MAX_JUMP + 1, 200) == PLUTO_RX_BAD);

  /* untimed stream: counted time, no stamp slot */
  struct pluto_geom gu;
  pluto_geom_init(&gu, 0, 256);
  uint8_t ub[256 * 4];
  memset(ub, 0, sizeof(ub));
  struct fake_rx fu = {ub, sizeof(ub), 1, 0, 0, 0};
  struct pluto_rx_source su = {&fu, fake_next, fake_release};
  pluto_rx_reader_init(&r, &gu);
  CHECK(pluto_rx_read(&r, &su, PLUTO_FMT_CS16, buf, 512, &has_time, &s, 0) == 256 && !has_time && s == 0);
  free(f.blocks);
}

/* ---------- PL TX model: util_upack2_timestamp ---------- */

#define MAX_BLOCKS 256
struct fake_tx {
  uint8_t *pool;
  uint32_t block_bytes, nslots;
  uint32_t committed, acquired_slot;
  int holding, deny;
};
static int fake_acquire(void *ctx, uint8_t **b, long timeout_us)
{
  (void)timeout_us;
  struct fake_tx *f = ctx;
  if (f->deny || f->committed >= f->nslots)
    return -1;
  CHECK(!f->holding);
  f->holding = 1;
  /* poison so missing padding shows */
  memset(f->pool + (size_t)f->committed * f->block_bytes, 0x5a, f->block_bytes);
  *b = f->pool + (size_t)f->committed * f->block_bytes;
  return 0;
}
static void fake_commit(void *ctx)
{
  struct fake_tx *f = ctx;
  CHECK(f->holding);
  f->holding = 0;
  f->committed++;
}

/* Play committed blocks through the upack model: each block is [stamp][E words]; it is discarded
 * when its stamp is behind the counter or more than 16*E samples ahead, else held until the
 * counter reaches the stamp and played contiguously. out[t] = sample played at counter t. */
struct played {
  int16_t i, q;
  int set;
};
static uint32_t upack_play(const struct fake_tx *f, const struct pluto_geom *g, uint64_t counter0,
                           struct played *out, uint64_t out_len, uint64_t *counter_end)
{
  uint32_t discarded = 0;
  uint64_t counter = counter0;
  for (uint32_t b = 0; b < f->committed; ++b) {
    const uint8_t *blk = f->pool + (size_t)b * f->block_bytes;
    uint64_t st = pluto_stamp_get(blk);
    if (st < counter || st > counter + (uint64_t)(g->every / 2) * 16u) {
      discarded++;
      continue;
    }
    counter = st;
    for (uint32_t k = 0; k < g->data_samples; ++k, ++counter) {
      const uint8_t *p = blk + 8 + 4 * k;
      if (counter < out_len) {
        out[counter].i = (int16_t)(p[0] | (p[1] << 8));
        out[counter].q = (int16_t)(p[2] | (p[3] << 8));
        out[counter].set = 1;
      }
    }
  }
  *counter_end = counter;
  return discarded;
}

static void test_tx_writer(void)
{
  const uint32_t every = 200;
  struct pluto_geom g;
  pluto_geom_init(&g, every, 0);
  struct fake_tx f = {0};
  f.block_bytes = g.block_bytes;
  f.nslots = MAX_BLOCKS;
  f.pool = calloc(MAX_BLOCKS, g.block_bytes);
  struct pluto_tx_sink sink = {&f, fake_acquire, fake_commit};
  struct pluto_tx_writer w;
  pluto_tx_writer_init(&w, &g);

  /* nexus2 style: timed contiguous writes of odd sizes starting at 10000 */
  int16_t src[2 * 1000];
  uint64_t t0 = 10000, t = t0;
  const size_t sizes[] = {150, 50, 333, 17, 250, 200, 1000};
  for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
    for (size_t k = 0; k < sizes[s]; ++k) {
      src[2 * k] = i_of(t + k);
      src[2 * k + 1] = q_of(t + k);
    }
    size_t done = 0;
    while (done < sizes[s]) {
      int n = pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src + 2 * done, sizes[s] - done, 1, t + done, 0, 0);
      CHECK(n > 0);
      if (n <= 0)
        break;
      done += (size_t)n;
    }
    t += sizes[s];
  }
  /* end the burst: the partial block is committed zero padded */
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 0, 0, 0, 1, 0) == 0);
  uint64_t total = t - t0;
  CHECK(f.committed == (total + every - 1) / every);
  CHECK(!f.holding);
  for (uint32_t b = 0; b < f.committed; ++b)
    CHECK(pluto_stamp_get(f.pool + (size_t)b * g.block_bytes) == t0 + (uint64_t)b * every);

  static struct played out[40000];
  memset(out, 0, sizeof(out));
  uint64_t end;
  CHECK(upack_play(&f, &g, t0 - 50, out, 40000, &end) == 0);
  int wrong = 0;
  for (uint64_t c = t0; c < t; ++c)
    if (!out[c].set || out[c].i != i_of(c) || out[c].q != q_of(c))
      wrong++;
  CHECK(wrong == 0);
  /* the padding after the burst is zeros, nothing before t0 */
  for (uint64_t c = t; c < t0 + (uint64_t)f.committed * every; ++c)
    CHECK(out[c].set && out[c].i == 0 && out[c].q == 0);
  CHECK(!out[t0 - 1].set);

  /* gaps: a timed write ahead of the fill position pads zeros up to it, inside the block */
  f.committed = 0;
  pluto_tx_writer_init(&w, &g);
  for (int k = 0; k < 2 * 1000; ++k)
    src[k] = 1000;
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 50, 1, 20000, 0, 0) == 50);
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 50, 1, 20080, 0, 0) == 50); /* 30 zeros */
  CHECK(f.committed == 0);
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 10, 1, 20195, 0, 0) == 5); /* 65 zeros, 5 fit */
  CHECK(f.committed == 1);
  /* jump past the block: the rest is padded and a new block starts at the new time */
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 10, 1, 20700, 0, 0) == 10);
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 1, 1, 21000, 1, 0) == 1);
  CHECK(f.committed == 3);
  CHECK(pluto_stamp_get(f.pool) == 20000);
  CHECK(pluto_stamp_get(f.pool + g.block_bytes) == 20700);
  CHECK(pluto_stamp_get(f.pool + 2 * g.block_bytes) == 21000);
  memset(out, 0, sizeof(out));
  CHECK(upack_play(&f, &g, 19000, out, 40000, &end) == 0);
  CHECK(out[20000].i == 1000 && out[20049].i == 1000 && out[20050].i == 0 && out[20079].i == 0);
  CHECK(out[20080].i == 1000 && out[20129].i == 1000 && out[20130].i == 0 && out[20194].i == 0);
  CHECK(out[20195].i == 1000 && out[20199].i == 1000);
  CHECK(out[20700].i == 1000 && out[20709].i == 1000 && out[20710].i == 0);

  /* a time behind the fill position pads the rest and starts again at that time (gadget) */
  f.committed = 0;
  pluto_tx_writer_init(&w, &g);
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 100, 1, 30000, 0, 0) == 100);
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 100, 1, 29950, 0, 0) == 100);
  CHECK(f.committed == 1 && w.backwards == 1);
  CHECK(pluto_stamp_get(f.pool + 0) == 30000 && pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 0, 0, 0, 1, 0) == 0);
  CHECK(pluto_stamp_get(f.pool + g.block_bytes) == 29950);
  /* played: the second block overlaps -> the upack discards it as late */
  memset(out, 0, sizeof(out));
  CHECK(upack_play(&f, &g, 29000, out, 40000, &end) == 1);

  /* untimed writes continue the time of the previous block */
  f.committed = 0;
  pluto_tx_writer_init(&w, &g);
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 200, 1, 50000, 0, 0) == 200);
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 200, 0, 0, 0, 0) == 200);
  CHECK(f.committed == 2 && pluto_stamp_get(f.pool + g.block_bytes) == 50200);

  /* too early (> 8 blocks ahead) and late blocks are discarded by the FPGA */
  f.committed = 0;
  pluto_tx_writer_init(&w, &g);
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 200, 1, 60000 + 1601, 0, 0) == 200);
  memset(out, 0, sizeof(out));
  CHECK(upack_play(&f, &g, 60000, out, 1, &end) == 1);
  f.committed = 0;
  pluto_tx_writer_init(&w, &g);
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 200, 1, 60000 + 1600, 0, 0) == 200);
  CHECK(upack_play(&f, &g, 60000, out, 1, &end) == 0);

  /* no free slot: timeout without consuming anything; the next call starts cleanly */
  f.committed = 0;
  pluto_tx_writer_init(&w, &g);
  f.deny = 1;
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 100, 1, 70000, 0, 0) == PLUTO_ERR_TIMEOUT);
  CHECK(w.cur == NULL && f.committed == 0);
  f.deny = 0;
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 100, 1, 70000, 0, 0) == 100);
  /* a full block plus a write that needs a new slot that is not there: partial count returned */
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 100, 1, 70100, 0, 0) == 100);
  CHECK(f.committed == 1);
  f.deny = 1;
  CHECK(pluto_tx_write(&w, &sink, PLUTO_FMT_CS16, src, 100, 1, 70200, 0, 0) == PLUTO_ERR_TIMEOUT);
  CHECK(f.committed == 1 && w.cur == NULL);
  f.deny = 0;

  /* CF32 / CS8 conversion like the gadget (x * 32767.999 truncated, cs8 << 8), clamped */
  float fsrc[8] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 0.000001f, -0.999999f};
  int16_t conv[8];
  pluto_tx_convert(PLUTO_FMT_CF32, conv, fsrc, 4);
  CHECK(conv[0] == (int16_t)(0.5f * 32767.999f) && conv[1] == (int16_t)(-0.5f * 32767.999f));
  CHECK(conv[2] == 32767 && conv[3] == -32767 && conv[4] == 32767 && conv[5] == -32768);
  CHECK(conv[6] == 0 && conv[7] == (int16_t)(-0.999999f * 32767.999f));
  int8_t c8[4] = {1, -1, 127, -128};
  pluto_tx_convert(PLUTO_FMT_CS8, conv, c8, 2);
  CHECK(conv[0] == 256 && conv[1] == -256 && conv[2] == 32512 && conv[3] == -32768);
  free(f.pool);
}

int main(void)
{
  test_geometry();
  test_stamp_bytes();
  test_time();
  test_rx_reader();
  test_tx_writer();
  printf("test_framing: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
