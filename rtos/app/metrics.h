/*
 * Board metrics for the Mac-side tools (tools/pluto): GET /metrics, GET /log, /cmd on the
 * port 80 status server (http_status.c). Everything is computed on request in the HTTP thread
 * (CPU0, low priority); nothing here runs periodically or touches the radio hot path.
 *
 * metrics_fmt.c holds the pure parts (JSON writer, nexus2 stage table parser, log escaping) and
 * builds on the host for tests; metrics.c the RTEMS collectors; metrics_usb.c the CherryUSB
 * counters (USB_STACK=cherry images only, built with the CherryUSB include path).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEXUS_METRICS_H
#define NEXUS_METRICS_H

#include <stddef.h>
#include <stdint.h>

/* JSON schema of GET /metrics; bump on incompatible changes (tools/pluto checks the major) */
#define NEXUS_METRICS_SCHEMA "nexus.metrics/1"

/* ---- JSON writer into a fixed buffer (metrics_fmt.c) ---- */
#define JW_DEPTH 12
typedef struct {
  char *buf;
  size_t cap, len;
  int depth;
  unsigned char first[JW_DEPTH]; /* no member written yet at this depth */
  int overflow;                  /* output did not fit: the document is unusable */
} jw_t;

void jw_init(jw_t *w, char *buf, size_t cap);
/* key NULL inside an array (and for the root object) */
void jw_obj(jw_t *w, const char *key);
void jw_obj_end(jw_t *w);
void jw_arr(jw_t *w, const char *key);
void jw_arr_end(jw_t *w);
void jw_str(jw_t *w, const char *key, const char *v); /* v NULL: null */
/* n bytes of arbitrary text (log ring): control bytes escaped, invalid UTF-8 -> U+FFFD */
void jw_strn(jw_t *w, const char *key, const char *v, size_t n);
void jw_int(jw_t *w, const char *key, long long v);
void jw_uint(jw_t *w, const char *key, unsigned long long v);
/* fixed decimals; NaN/inf -> null */
void jw_num(jw_t *w, const char *key, double v, int decimals);
void jw_bool(jw_t *w, const char *key, int v);
void jw_null(jw_t *w, const char *key);
/* NUL-terminates; returns the length, 0 when the buffer overflowed or nesting is open */
size_t jw_finish(jw_t *w);

/*
 * nexus2's `nexus stages` table (rf-front stage_timing.rs report()) as JSON members of the
 * current object: "items": [{name, count, mean_us, p99_us, max_us, budget_us, over, us_per_s}],
 * "tx_late": n. Returns the number of stage rows.
 */
int metrics_stage_table_json(jw_t *w, const char *report);

/* /cmd allowlist (http_status.c): 1 when argv (split on spaces) may run */
int metrics_cmd_allowed(int argc, char **argv);

/* ---- collectors (metrics.c) ---- */

/* request options, from the query string */
typedef struct {
  int top;      /* threads listed (default 12, max 64) */
  int stacks;   /* stack high-water marks: walks every stack, so on request only (full=1) */
  int events;   /* `net status` recent events (full=1) */
  int hw;       /* AD9363 readback through the driver lock (default 1; hw=0 skips) */
} metrics_opts;

/* the whole /metrics document; returns its length, 0 if it did not fit */
size_t metrics_json(char *buf, size_t cap, const metrics_opts *o);

/*
 * Run one registered RTEMS shell command in the calling thread with its stdout captured into
 * buf (NUL-terminated). Returns the command's exit status, or -1 when it does not exist or the
 * capture could not be set up. Commands must only print (no stdin, no fileno(stdout) writes).
 */
int metrics_shell_capture(int argc, char **argv, char *buf, size_t cap);

/* ---- CherryUSB counters (metrics_usb.c; referenced weakly by metrics.c: absent in libbsd USB
 * images) ---- */
void metrics_usb_json(jw_t *w);

/* ---- console log ring (posix_compat.c, nexus2 images only; referenced weakly) ----
 * Copies ring bytes from the free-running offset `since` (bytes ever written) into buf; an
 * offset older than the ring starts at the oldest byte held. Returns the bytes copied;
 * *next = offset after them, *oldest = oldest offset still held. */
size_t nexus_log_read(uint32_t since, char *buf, size_t cap, uint32_t *next, uint32_t *oldest);

/* ---- HTTP server thread (http_status.c), started by Init ---- */
void *nexus_http_status(void *arg);
/* the plain-text page of GET / (main.c) */
int nexus_status_text(char *buf, size_t cap);

#endif
