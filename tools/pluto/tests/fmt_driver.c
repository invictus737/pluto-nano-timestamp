/*
 * Host driver for rtos/app/metrics_fmt.c (test_board_fmt.py builds it with the host cc):
 *   fmt_driver stages <file>   stage table text -> {"items": [...], "tx_late": n}
 *   fmt_driver text <file>     arbitrary bytes -> {"text": "..."} (log ring escaping)
 *   fmt_driver doc             a document using every writer call
 *   fmt_driver overflow        the same document into a too small buffer (prints the length)
 *   fmt_driver allow <file>    one /cmd line per input line -> "1" or "0" per line
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metrics.h"

static char out[1 << 20];

static size_t slurp(const char *path, char *buf, size_t cap)
{
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    perror(path);
    exit(2);
  }
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = '\0';
  return n;
}

static size_t doc(char *buf, size_t cap)
{
  jw_t w;
  jw_init(&w, buf, cap);
  jw_obj(&w, NULL);
  jw_str(&w, "schema", NEXUS_METRICS_SCHEMA);
  jw_str(&w, "quote", "a\"b\\c\n\t\x01");
  jw_str(&w, "none", NULL);
  jw_int(&w, "neg", -42);
  jw_uint(&w, "big", 18446744073709551615ull);
  jw_num(&w, "pi", 3.14159, 2);
  jw_num(&w, "nan", NAN, 2);
  jw_bool(&w, "t", 1);
  jw_bool(&w, "f", 0);
  jw_null(&w, "n");
  jw_arr(&w, "arr");
  jw_int(&w, NULL, 1);
  jw_obj(&w, NULL);
  jw_arr(&w, "empty");
  jw_arr_end(&w);
  jw_obj_end(&w);
  jw_str(&w, NULL, "x");
  jw_arr_end(&w);
  jw_obj(&w, "empty_obj");
  jw_obj_end(&w);
  jw_obj_end(&w);
  return jw_finish(&w);
}

int main(int argc, char **argv)
{
  static char in[1 << 20];
  if (argc >= 3 && strcmp(argv[1], "stages") == 0) {
    slurp(argv[2], in, sizeof(in));
    jw_t w;
    jw_init(&w, out, sizeof(out));
    jw_obj(&w, NULL);
    int rows = metrics_stage_table_json(&w, in);
    jw_int(&w, "rows", rows);
    jw_obj_end(&w);
    size_t n = jw_finish(&w);
    if (n == 0) {
      return 1;
    }
    fwrite(out, 1, n, stdout);
    return 0;
  }
  if (argc >= 3 && strcmp(argv[1], "text") == 0) {
    size_t len = slurp(argv[2], in, sizeof(in));
    jw_t w;
    jw_init(&w, out, sizeof(out));
    jw_obj(&w, NULL);
    jw_strn(&w, "text", in, len);
    jw_obj_end(&w);
    size_t n = jw_finish(&w);
    if (n == 0) {
      return 1;
    }
    fwrite(out, 1, n, stdout);
    return 0;
  }
  if (argc >= 2 && strcmp(argv[1], "doc") == 0) {
    size_t n = doc(out, sizeof(out));
    fwrite(out, 1, n, stdout);
    return n == 0;
  }
  if (argc >= 3 && strcmp(argv[1], "allow") == 0) {
    slurp(argv[2], in, sizeof(in));
    char *save = NULL;
    for (char *l = strtok_r(in, "\n", &save); l != NULL; l = strtok_r(NULL, "\n", &save)) {
      char *av[16];
      int ac = 0;
      char *s2 = NULL;
      for (char *t = strtok_r(l, " ", &s2); t != NULL && ac < 15; t = strtok_r(NULL, " ", &s2)) {
        av[ac++] = t;
      }
      av[ac] = NULL;
      printf("%d\n", metrics_cmd_allowed(ac, av));
    }
    return 0;
  }
  if (argc >= 2 && strcmp(argv[1], "overflow") == 0) {
    char small[40];
    size_t n = doc(small, sizeof(small));
    printf("%zu %d\n", n, small[0]);
    return 0;
  }
  return 2;
}
