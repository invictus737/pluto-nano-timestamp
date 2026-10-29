/*
 * Pure parts of the board metrics (metrics.h): a JSON writer into a fixed buffer, the nexus2
 * stage table parser and text escaping for the log ring. No RTEMS dependency: built on the host
 * by tools/pluto/tests (test_board_fmt.py) as well.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metrics.h"

void jw_init(jw_t *w, char *buf, size_t cap)
{
  w->buf = buf;
  w->cap = cap;
  w->len = 0;
  w->depth = 0;
  w->first[0] = 1;
  w->overflow = cap == 0;
  if (cap > 0) {
    buf[0] = '\0';
  }
}

static void put(jw_t *w, const char *s, size_t n)
{
  if (w->overflow) {
    return;
  }
  if (w->len + n + 1 > w->cap) {
    w->overflow = 1;
    return;
  }
  memcpy(w->buf + w->len, s, n);
  w->len += n;
}

static void putc_(jw_t *w, char c)
{
  put(w, &c, 1);
}

static void putf(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void putf(jw_t *w, const char *fmt, ...)
{
  if (w->overflow) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  size_t room = w->cap - w->len;
  int n = vsnprintf(w->buf + w->len, room, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n + 1 > room) {
    w->overflow = 1;
    return;
  }
  w->len += (size_t)n;
}

/* one UTF-8 sequence at p (n bytes left): its length if valid, else 0 */
static size_t utf8_len(const unsigned char *p, size_t n)
{
  unsigned c = p[0];
  size_t len;
  unsigned min;
  if (c < 0x80) {
    return 1;
  } else if ((c & 0xe0) == 0xc0) {
    len = 2, min = 0x80;
  } else if ((c & 0xf0) == 0xe0) {
    len = 3, min = 0x800;
  } else if ((c & 0xf8) == 0xf0) {
    len = 4, min = 0x10000;
  } else {
    return 0;
  }
  if (len > n) {
    return 0;
  }
  unsigned cp = c & (0x7fu >> len);
  for (size_t i = 1; i < len; ++i) {
    if ((p[i] & 0xc0) != 0x80) {
      return 0;
    }
    cp = (cp << 6) | (p[i] & 0x3fu);
  }
  if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
    return 0;
  }
  return len;
}

static void put_escaped(jw_t *w, const char *s, size_t n)
{
  const unsigned char *p = (const unsigned char *)s;
  putc_(w, '"');
  size_t i = 0;
  while (i < n && !w->overflow) {
    unsigned c = p[i];
    if (c == '"' || c == '\\') {
      char e[2] = {'\\', (char)c};
      put(w, e, 2);
      ++i;
    } else if (c == '\n') {
      put(w, "\\n", 2);
      ++i;
    } else if (c == '\r') {
      put(w, "\\r", 2);
      ++i;
    } else if (c == '\t') {
      put(w, "\\t", 2);
      ++i;
    } else if (c < 0x20 || c == 0x7f) {
      putf(w, "\\u%04x", c);
      ++i;
    } else if (c < 0x80) {
      /* runs of plain ASCII in one copy */
      size_t j = i + 1;
      while (j < n && p[j] >= 0x20 && p[j] < 0x7f && p[j] != '"' && p[j] != '\\') {
        ++j;
      }
      put(w, s + i, j - i);
      i = j;
    } else {
      size_t l = utf8_len(p + i, n - i);
      if (l == 0) {
        put(w, "\\ufffd", 6); /* torn or binary byte */
        ++i;
      } else {
        put(w, s + i, l);
        i += l;
      }
    }
  }
  putc_(w, '"');
}

/* separator and "key": for the next member */
static void member(jw_t *w, const char *key)
{
  if (w->depth >= 0 && w->depth < JW_DEPTH) {
    if (!w->first[w->depth]) {
      putc_(w, ',');
    }
    w->first[w->depth] = 0;
  }
  if (key != NULL) {
    put_escaped(w, key, strlen(key));
    putc_(w, ':');
  }
}

static void open_(jw_t *w, const char *key, char c)
{
  member(w, key);
  putc_(w, c);
  if (w->depth + 1 >= JW_DEPTH) {
    w->overflow = 1;
    return;
  }
  w->first[++w->depth] = 1;
}

static void close_(jw_t *w, char c)
{
  if (w->depth <= 0) {
    w->overflow = 1;
    return;
  }
  --w->depth;
  putc_(w, c);
}

void jw_obj(jw_t *w, const char *key)
{
  open_(w, key, '{');
}

void jw_obj_end(jw_t *w)
{
  close_(w, '}');
}

void jw_arr(jw_t *w, const char *key)
{
  open_(w, key, '[');
}

void jw_arr_end(jw_t *w)
{
  close_(w, ']');
}

void jw_str(jw_t *w, const char *key, const char *v)
{
  if (v == NULL) {
    jw_null(w, key);
    return;
  }
  member(w, key);
  put_escaped(w, v, strlen(v));
}

void jw_strn(jw_t *w, const char *key, const char *v, size_t n)
{
  member(w, key);
  put_escaped(w, v, n);
}

void jw_int(jw_t *w, const char *key, long long v)
{
  member(w, key);
  putf(w, "%lld", v);
}

void jw_uint(jw_t *w, const char *key, unsigned long long v)
{
  member(w, key);
  putf(w, "%llu", v);
}

void jw_num(jw_t *w, const char *key, double v, int decimals)
{
  if (isnan(v) || isinf(v)) {
    jw_null(w, key);
    return;
  }
  member(w, key);
  putf(w, "%.*f", decimals < 0 ? 0 : decimals > 9 ? 9 : decimals, v);
}

void jw_bool(jw_t *w, const char *key, int v)
{
  member(w, key);
  put(w, v ? "true" : "false", v ? 4 : 5);
}

void jw_null(jw_t *w, const char *key)
{
  member(w, key);
  put(w, "null", 4);
}

size_t jw_finish(jw_t *w)
{
  if (w->overflow || w->depth != 0 || w->cap == 0) {
    if (w->cap > 0) {
      w->buf[0] = '\0';
    }
    return 0;
  }
  w->buf[w->len] = '\0';
  return w->len;
}

/* ---- nexus2 stage table ----
 * Header "stage count mean_us p99_us max_us budget over us/s", then one row per active stage
 * ("rx.analysis 12345 410.2 512 2048 0 0 41022"), "tx.late N blocks skipped since start" and a
 * legend line. Rows are recognised by shape (name + 7 numbers), so a reordered or extended
 * legend does not break the parser. */
int metrics_stage_table_json(jw_t *w, const char *report)
{
  int rows = 0;
  long long tx_late = -1;
  jw_arr(w, "items");
  const char *line = report;
  while (line != NULL && *line != '\0') {
    const char *nl = strchr(line, '\n');
    size_t n = nl != NULL ? (size_t)(nl - line) : strlen(line);
    char l[256];
    if (n >= sizeof(l)) {
      n = sizeof(l) - 1;
    }
    memcpy(l, line, n);
    l[n] = '\0';
    char name[64];
    unsigned long long count, p99, max, budget, over;
    double mean, usps;
    char tail;
    if (sscanf(l, "%63s %llu %lf %llu %llu %llu %llu %lf %c", name, &count, &mean, &p99, &max, &budget,
               &over, &usps, &tail) == 8) {
      jw_obj(w, NULL);
      jw_str(w, "name", name);
      jw_uint(w, "count", count);
      jw_num(w, "mean_us", mean, 1);
      jw_uint(w, "p99_us", p99);
      jw_uint(w, "max_us", max);
      jw_uint(w, "budget_us", budget);
      jw_uint(w, "over", over);
      jw_num(w, "us_per_s", usps, 0);
      jw_obj_end(w);
      ++rows;
    } else {
      unsigned long long late;
      if (sscanf(l, "tx.late %llu", &late) == 1) {
        tx_late = (long long)late;
      }
    }
    line = nl != NULL ? nl + 1 : NULL;
  }
  jw_arr_end(w);
  if (tx_late >= 0) {
    jw_int(w, "tx_late", tx_late);
  } else {
    jw_null(w, "tx_late");
  }
  return rows;
}

/* ---- /cmd allowlist (http_status.c) ----
 * nexus start|stop|status|layout [light|cpu1|float]|env [pluto|save|clear|NEXUS2_*=V|RUST_LOG=V
 * ...]|stages [reset]|load [1..10]; usbh; lsusb [-t]; net status; wifi status. Words are limited
 * to [A-Za-z0-9_=.:,/-], so nothing reaches a command that the shell would treat specially. */
static int word_ok(const char *s)
{
  if (*s == '\0' || strlen(s) > 120) {
    return 0;
  }
  for (; *s; ++s) {
    char ch = *s;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
          ch == '=' || ch == '.' || ch == ':' || ch == '-' || ch == ',' || ch == '/')) {
      return 0;
    }
  }
  return 1;
}

static int is_env_kv(const char *s)
{
  return (strncmp(s, "NEXUS2_", 7) == 0 || strncmp(s, "RUST_LOG=", 9) == 0) && strchr(s, '=') != NULL;
}

int metrics_cmd_allowed(int argc, char **argv)
{
  if (argc < 1) {
    return 0;
  }
  for (int i = 0; i < argc; ++i) {
    if (!word_ok(argv[i])) {
      return 0;
    }
  }
  const char *c = argv[0], *s = argc > 1 ? argv[1] : "";
  if (strcmp(c, "usbh") == 0) {
    return argc == 1;
  }
  if (strcmp(c, "lsusb") == 0) {
    return argc == 1 || (argc == 2 && strcmp(s, "-t") == 0);
  }
  if (strcmp(c, "net") == 0 || strcmp(c, "wifi") == 0) {
    return argc == 2 && strcmp(s, "status") == 0;
  }
  if (strcmp(c, "nexus") != 0 || argc < 2) {
    return 0;
  }
  if (strcmp(s, "start") == 0 || strcmp(s, "stop") == 0 || strcmp(s, "status") == 0) {
    return argc == 2;
  }
  if (strcmp(s, "layout") == 0) {
    return argc == 2 || (argc == 3 && (strcmp(argv[2], "light") == 0 || strcmp(argv[2], "cpu1") == 0 ||
                                       strcmp(argv[2], "float") == 0));
  }
  if (strcmp(s, "stages") == 0) {
    return argc == 2 || (argc == 3 && strcmp(argv[2], "reset") == 0);
  }
  if (strcmp(s, "load") == 0) {
    if (argc == 2) {
      return 1;
    }
    char *end;
    long v = strtol(argv[2], &end, 10);
    return argc == 3 && *end == '\0' && v >= 1 && v <= 10;
  }
  if (strcmp(s, "env") == 0) {
    if (argc == 2) {
      return 1;
    }
    if (argc == 3 && (strcmp(argv[2], "pluto") == 0 || strcmp(argv[2], "save") == 0 ||
                      strcmp(argv[2], "clear") == 0)) {
      return 1;
    }
    for (int i = 2; i < argc; ++i) {
      if (!is_env_kv(argv[i])) {
        return 0;
      }
    }
    return 1;
  }
  return 0;
}

