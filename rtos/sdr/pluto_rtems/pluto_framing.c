/*
 * Pluto timestamp framing, sample conversion and the RX reader / TX writer state machines.
 * See pluto_framing.h for the hardware contract. Platform independent.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pluto_framing.h"

#include <math.h>
#include <string.h>

int pluto_geom_init(struct pluto_geom *g, uint32_t timestamp_every, uint32_t untimed_len)
{
  memset(g, 0, sizeof(*g));
  if (timestamp_every > 0) {
    if ((timestamp_every & 1u) || timestamp_every > PLUTO_MAX_EVERY)
      return -1;
    g->every = timestamp_every;
    g->data_samples = timestamp_every;
    g->block_samples = timestamp_every + PLUTO_STAMP_SAMPLES;
    g->stamp_bytes = PLUTO_WORD_BYTES;
    g->gpio_words = timestamp_every / 2u;
  } else {
    if (untimed_len == 0 || (untimed_len & 1u) || untimed_len > PLUTO_MAX_EVERY)
      return -1;
    g->data_samples = untimed_len;
    g->block_samples = untimed_len;
  }
  g->block_bytes = g->block_samples * PLUTO_SAMPLE_BYTES;
  return 0;
}

uint32_t pluto_gpio_with_every(uint32_t current, uint32_t words)
{
  return (current & 1u) | (words << 1);
}

uint32_t pluto_gpio_with_fir(uint32_t current, int fir_enable)
{
  return (current & ~1u) | (fir_enable ? 1u : 0u);
}

uint64_t pluto_stamp_get(const void *block)
{
  const uint8_t *b = (const uint8_t *)block;
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i)
    v = (v << 8) | b[i];
  return v;
}

void pluto_stamp_put(void *block, uint64_t stamp)
{
  uint8_t *b = (uint8_t *)block;
  for (int i = 0; i < 8; ++i)
    b[i] = (uint8_t)(stamp >> (8 * i));
}

long long pluto_ticks_to_ns(long long ticks, double rate)
{
  const long long ratell = (long long)(rate);
  const long long full = (long long)(ticks / ratell);
  const long long err = ticks - (full * ratell);
  const double part = full * (rate - ratell);
  const double frac = ((err - part) * 1000000000) / rate;
  return (full * 1000000000) + llround(frac);
}

long long pluto_ns_to_ticks(long long time_ns, double rate)
{
  const long long ratell = (long long)(rate);
  const long long full = (long long)(time_ns / 1000000000);
  const long long err = time_ns - (full * 1000000000);
  const double part = full * (rate - ratell);
  const double frac = part + ((err * rate) / 1000000000);
  return (full * ratell) + llround(frac);
}

void pluto_rx_track_reset(struct pluto_rx_track *t)
{
  t->have = 0;
  t->expected = 0;
}

enum pluto_rx_verdict pluto_rx_track_block(struct pluto_rx_track *t, uint64_t stamp,
                                           uint32_t data_samples)
{
  enum pluto_rx_verdict v;
  if (!t->have) {
    v = PLUTO_RX_FIRST;
  } else if (stamp == t->expected) {
    v = PLUTO_RX_OK;
  } else if (stamp > t->expected && stamp - t->expected <= PLUTO_RX_MAX_JUMP) {
    v = PLUTO_RX_GAP;
    t->overflows++;
    t->lost_samples += stamp - t->expected;
  } else {
    /* not a stamp: the transfer did not start on a stamp word. The next good block restarts
     * the timeline as FIRST. */
    t->misframed++;
    t->have = 0;
    return PLUTO_RX_BAD;
  }
  t->blocks++;
  t->have = 1;
  t->expected = stamp + data_samples;
  return v;
}

size_t pluto_format_bytes(enum pluto_format f)
{
  switch (f) {
  case PLUTO_FMT_CS16: return 4;
  case PLUTO_FMT_CF32: return 8;
  case PLUTO_FMT_CS8: return 2;
  }
  return 0;
}

static int16_t rd16(const uint8_t *p)
{
  return (int16_t)(uint16_t)(p[0] | (p[1] << 8));
}

static void wr16(uint8_t *p, int16_t v)
{
  p[0] = (uint8_t)((uint16_t)v);
  p[1] = (uint8_t)((uint16_t)v >> 8);
}

void pluto_rx_convert(enum pluto_format f, void *dst, const void *src, size_t samples)
{
  const uint8_t *s = (const uint8_t *)src;
  switch (f) {
  case PLUTO_FMT_CS16:
    /* bus layout == SoapySDR CS16 on a little endian CPU (SoapyPlutoSDR direct copy) */
    memcpy(dst, src, samples * 4);
    break;
  case PLUTO_FMT_CF32: {
    float *d = (float *)dst;
    for (size_t i = 0; i < samples * 2; ++i)
      d[i] = (float)rd16(s + 2 * i) / PLUTO_RX_FULL_SCALE;
    break;
  }
  case PLUTO_FMT_CS8: {
    int8_t *d = (int8_t *)dst;
    for (size_t i = 0; i < samples * 2; ++i)
      d[i] = (int8_t)(rd16(s + 2 * i) >> 4);
    break;
  }
  }
}

static int16_t f2s16(float x)
{
  /* SoapyPlutoSDR: (int16_t)(x * 32767.999f), truncation; clamped here instead of the cast's
   * undefined behaviour outside [-1, 1) */
  float v = x * 32767.999f;
  if (!(v > -32768.0f))
    return v != v ? 0 : -32768;
  if (v >= 32767.0f)
    return 32767;
  return (int16_t)v;
}

void pluto_tx_convert(enum pluto_format f, void *dst, const void *src, size_t samples)
{
  uint8_t *d = (uint8_t *)dst;
  switch (f) {
  case PLUTO_FMT_CS16:
    memcpy(dst, src, samples * 4);
    break;
  case PLUTO_FMT_CF32: {
    const float *s = (const float *)src;
    for (size_t i = 0; i < samples * 2; ++i)
      wr16(d + 2 * i, f2s16(s[i]));
    break;
  }
  case PLUTO_FMT_CS8: {
    const int8_t *s = (const int8_t *)src;
    for (size_t i = 0; i < samples * 2; ++i)
      wr16(d + 2 * i, (int16_t)(s[i] * 256));
    break;
  }
  }
}

/* ---- RX reader ---- */

void pluto_rx_reader_init(struct pluto_rx_reader *r, const struct pluto_geom *g)
{
  memset(r, 0, sizeof(*r));
  r->geom = *g;
  pluto_rx_track_reset(&r->track);
}

void pluto_rx_reader_reset(struct pluto_rx_reader *r, const struct pluto_rx_source *src)
{
  if (r->cur && src)
    src->release(src->ctx);
  r->cur = NULL;
  r->remaining = 0;
  pluto_rx_track_reset(&r->track);
}

int pluto_rx_read(struct pluto_rx_reader *r, const struct pluto_rx_source *src, enum pluto_format f,
                  void *dst, size_t n, int *has_time, uint64_t *stamp, long timeout_us)
{
  const struct pluto_geom *g = &r->geom;
  if (!r->cur) {
    const uint8_t *b;
    if (src->next(src->ctx, &b, timeout_us) != 0)
      return PLUTO_ERR_TIMEOUT;
    if (g->every) {
      uint64_t s = pluto_stamp_get(b);
      if (pluto_rx_track_block(&r->track, s, g->data_samples) == PLUTO_RX_BAD) {
        src->release(src->ctx);
        return PLUTO_ERR_FRAMING;
      }
      r->stamp = s;
    } else {
      r->stamp = r->untimed_count;
      r->untimed_count += g->data_samples;
    }
    r->cur = b;
    r->remaining = g->data_samples;
    r->offset = g->stamp_bytes;
  }
  size_t items = n < r->remaining ? n : r->remaining;
  pluto_rx_convert(f, dst, r->cur + r->offset, items);
  *has_time = g->every != 0;
  *stamp = r->stamp;
  r->stamp += items;
  r->remaining -= (uint32_t)items;
  r->offset += (uint32_t)(items * PLUTO_SAMPLE_BYTES);
  if (r->remaining == 0) {
    r->cur = NULL;
    src->release(src->ctx);
  }
  return (int)items;
}

/* ---- TX writer ---- */

void pluto_tx_writer_init(struct pluto_tx_writer *w, const struct pluto_geom *g)
{
  memset(w, 0, sizeof(*w));
  w->geom = *g;
}

void pluto_tx_writer_reset(struct pluto_tx_writer *w)
{
  w->cur = NULL;
  w->stored = 0;
}

static void commit_block(struct pluto_tx_writer *w, const struct pluto_tx_sink *sink)
{
  const struct pluto_geom *g = &w->geom;
  if (w->stored < g->block_samples)
    memset(w->cur + (size_t)w->stored * PLUTO_SAMPLE_BYTES, 0,
           (size_t)(g->block_samples - w->stored) * PLUTO_SAMPLE_BYTES);
  w->cur = NULL;
  w->stored = 0;
  w->blocks++;
  sink->commit(sink->ctx);
}

void pluto_tx_writer_flush(struct pluto_tx_writer *w, const struct pluto_tx_sink *sink)
{
  if (w->cur)
    commit_block(w, sink);
}

int pluto_tx_write(struct pluto_tx_writer *w, const struct pluto_tx_sink *sink, enum pluto_format f,
                   const void *src, size_t n, int has_time, uint64_t stamp, int end_burst,
                   long timeout_us)
{
  const struct pluto_geom *g = &w->geom;
  if (w->cur && g->every && has_time) {
    /* gadget: pad zeros up to the requested time; a time behind the fill position (unsigned
     * difference) pads the whole rest of the block */
    uint64_t diff = stamp - w->next_stamp;
    uint64_t free_slots = g->block_samples - w->stored;
    uint64_t pad = diff < free_slots ? diff : free_slots;
    if (pad) {
      memset(w->cur + (size_t)w->stored * PLUTO_SAMPLE_BYTES, 0, (size_t)pad * PLUTO_SAMPLE_BYTES);
      w->stored += (uint32_t)pad;
      w->next_stamp += pad;
    }
    if (w->stored == g->block_samples)
      commit_block(w, sink);
  }
  if (!w->cur) {
    if (n == 0)
      return 0;
    uint8_t *b;
    if (sink->acquire(sink->ctx, &b, timeout_us) != 0)
      return PLUTO_ERR_TIMEOUT;
    w->cur = b;
    w->stored = 0;
    if (g->every) {
      if (has_time) {
        if (stamp < w->next_stamp)
          w->backwards++;
        w->next_stamp = stamp;
      }
      pluto_stamp_put(b, w->next_stamp);
      w->stored = PLUTO_STAMP_SAMPLES;
    }
  }
  size_t room = g->block_samples - w->stored;
  size_t items = n < room ? n : room;
  pluto_tx_convert(f, w->cur + (size_t)w->stored * PLUTO_SAMPLE_BYTES, src, items);
  w->stored += (uint32_t)items;
  w->next_stamp += items;
  if (w->stored == g->block_samples || (end_burst && items == n))
    commit_block(w, sink);
  return (int)items;
}

/* ---- level meter ---- */

void pluto_level_reset(struct pluto_level *l)
{
  memset(l, 0, sizeof(*l));
}

void pluto_level_add(struct pluto_level *l, enum pluto_format f, const void *buf, size_t samples)
{
  const size_t n = samples * 2;
  switch (f) {
  case PLUTO_FMT_CS16: {
    const int16_t *v = (const int16_t *)buf;
    uint64_t ss = 0;
    uint32_t pk = l->peak_i;
    for (size_t i = 0; i < n; ++i) {
      int32_t x = v[i];
      uint32_t a = (uint32_t)(x < 0 ? -x : x);
      ss += (uint64_t)((int64_t)x * x);
      if (a > pk)
        pk = a;
    }
    l->sumsq_i += ss;
    l->peak_i = pk;
    break;
  }
  case PLUTO_FMT_CS8: {
    const int8_t *v = (const int8_t *)buf;
    uint64_t ss = 0;
    uint32_t pk = l->peak_i;
    for (size_t i = 0; i < n; ++i) {
      int32_t x = v[i];
      uint32_t a = (uint32_t)(x < 0 ? -x : x);
      ss += (uint64_t)(x * x);
      if (a > pk)
        pk = a;
    }
    l->sumsq_i += ss;
    l->peak_i = pk;
    break;
  }
  case PLUTO_FMT_CF32: {
    const float *v = (const float *)buf;
    float ss = 0, pk = l->peak_f;
    for (size_t i = 0; i < n; ++i) {
      float x = v[i];
      ss += x * x;
      float a = fabsf(x);
      if (a > pk)
        pk = a;
    }
    l->sumsq_f += ss;
    l->peak_f = pk;
    break;
  }
  }
  l->values += n;
}

int pluto_level_dbfs(const struct pluto_level *l, enum pluto_format f, float full_scale_cs16,
                     double *peak_dbfs, double *rms_dbfs)
{
  if (l->values < 2)
    return -1;
  double fs = f == PLUTO_FMT_CF32 ? 1.0 : f == PLUTO_FMT_CS8 ? 128.0 : (double)full_scale_cs16;
  double ss = f == PLUTO_FMT_CF32 ? l->sumsq_f : (double)l->sumsq_i;
  double pk = f == PLUTO_FMT_CF32 ? (double)l->peak_f : (double)l->peak_i;
  double power = ss / ((double)l->values / 2.0) / (fs * fs); /* mean |x|^2 per complex sample */
  *rms_dbfs = power > 0 ? 10.0 * log10(power) : -200.0;
  *peak_dbfs = pk > 0 ? 20.0 * log10(pk / fs) : -200.0;
  return 0;
}
