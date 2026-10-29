/*
 * Portable part of the Nexus-BS RTOS configuration (see config_kv.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "config_kv.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- ordered key/value list ---- */

void kv_init(kv_t *kv)
{
  memset(kv, 0, sizeof(*kv));
}

void kv_free(kv_t *kv)
{
  for (size_t i = 0; i < kv->n; ++i) {
    free(kv->k[i]);
    free(kv->v[i]);
  }
  free(kv->k);
  free(kv->v);
  kv_init(kv);
}

static long kv_find(const kv_t *kv, const char *key)
{
  for (size_t i = 0; i < kv->n; ++i) {
    if (strcmp(kv->k[i], key) == 0) {
      return (long)i;
    }
  }
  return -1;
}

const char *kv_get(const kv_t *kv, const char *key)
{
  long i = kv_find(kv, key);
  return i < 0 ? NULL : kv->v[i];
}

int kv_set(kv_t *kv, const char *key, const char *value)
{
  long i = kv_find(kv, key);
  if (value == NULL || value[0] == '\0') {
    if (i < 0) {
      return 0;
    }
    free(kv->k[i]);
    free(kv->v[i]);
    memmove(&kv->k[i], &kv->k[i + 1], (kv->n - (size_t)i - 1) * sizeof(char *));
    memmove(&kv->v[i], &kv->v[i + 1], (kv->n - (size_t)i - 1) * sizeof(char *));
    kv->n--;
    return 1;
  }
  if (i >= 0) {
    if (strcmp(kv->v[i], value) == 0) {
      return 0;
    }
    char *nv = strdup(value);
    if (nv == NULL) {
      return -1;
    }
    free(kv->v[i]);
    kv->v[i] = nv;
    return 1;
  }
  if (kv->n == kv->cap) {
    size_t cap = kv->cap ? kv->cap * 2 : 16;
    char **nk = realloc(kv->k, cap * sizeof(char *));
    if (nk == NULL) {
      return -1;
    }
    kv->k = nk;
    char **nv = realloc(kv->v, cap * sizeof(char *));
    if (nv == NULL) {
      return -1;
    }
    kv->v = nv;
    kv->cap = cap;
  }
  char *k = strdup(key);
  char *v = strdup(value);
  if (k == NULL || v == NULL) {
    free(k);
    free(v);
    return -1;
  }
  kv->k[kv->n] = k;
  kv->v[kv->n] = v;
  kv->n++;
  return 1;
}

int kv_copy(kv_t *dst, const kv_t *src)
{
  kv_init(dst);
  for (size_t i = 0; i < src->n; ++i) {
    if (kv_set(dst, src->k[i], src->v[i]) < 0) {
      kv_free(dst);
      return -1;
    }
  }
  return 0;
}

/* ---- growable text buffer ---- */

typedef struct {
  char *p;
  size_t n, cap;
  int err;
} sbuf_t;

static void sb_add(sbuf_t *b, const char *fmt, ...)
{
  if (b->err) {
    return;
  }
  for (;;) {
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
    va_end(ap);
    if (w < 0) {
      b->err = 1;
      return;
    }
    if (b->p != NULL && (size_t)w < b->cap - b->n) {
      b->n += (size_t)w;
      return;
    }
    size_t cap = b->cap ? b->cap : 256;
    while (cap <= b->n + (size_t)w) {
      cap *= 2;
    }
    char *np = realloc(b->p, cap);
    if (np == NULL) {
      b->err = 1;
      return;
    }
    b->p = np;
    b->cap = cap;
  }
}

static char *sb_done(sbuf_t *b)
{
  if (b->err) {
    free(b->p);
    return NULL;
  }
  if (b->p == NULL) {
    return strdup("");
  }
  return b->p;
}

/* ---- validators: normalize in place, return 0 when valid ---- */

typedef int (*valid_fn)(char *v);

static int parse_ipv4(const char *s, const char **end)
{
  int parts = 0;
  const char *p = s;
  while (parts < 4) {
    if (!isdigit((unsigned char)*p)) {
      return -1;
    }
    long n = 0;
    int digits = 0;
    while (isdigit((unsigned char)*p)) {
      n = n * 10 + (*p - '0');
      ++p;
      if (++digits > 3) {
        return -1;
      }
    }
    if (n > 255) {
      return -1;
    }
    if (++parts < 4) {
      if (*p != '.') {
        return -1;
      }
      ++p;
    }
  }
  *end = p;
  return 0;
}

static int v_ipv4(char *v)
{
  const char *e;
  return parse_ipv4(v, &e) == 0 && *e == '\0' ? 0 : -1;
}

/* a.b.c.d or a.b.c.d/nn */
static int v_ipv4_prefix(char *v)
{
  const char *e;
  if (parse_ipv4(v, &e) != 0) {
    return -1;
  }
  if (*e == '\0') {
    return 0;
  }
  if (*e != '/' || !isdigit((unsigned char)e[1])) {
    return -1;
  }
  char *x;
  long pfx = strtol(e + 1, &x, 10);
  return *x == '\0' && pfx >= 1 && pfx <= 32 ? 0 : -1;
}

static int v_netmask(char *v)
{
  const char *e;
  unsigned a, b, c, d;
  if (parse_ipv4(v, &e) != 0 || *e != '\0' || sscanf(v, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
    return -1;
  }
  uint32_t m = (a << 24) | (b << 16) | (c << 8) | d;
  uint32_t inv = ~m;
  return (inv & (inv + 1)) == 0 ? 0 : -1; /* contiguous ones */
}

/* one or more IPv4 addresses separated by spaces or commas -> "a b c" */
static int v_dns(char *v)
{
  char out[256];
  size_t o = 0;
  char *save = NULL;
  int n = 0;
  char tmp[256];
  if (strlen(v) >= sizeof(tmp)) {
    return -1;
  }
  strcpy(tmp, v);
  for (char *t = strtok_r(tmp, " ,\t", &save); t != NULL; t = strtok_r(NULL, " ,\t", &save)) {
    if (v_ipv4(t) != 0 || ++n > 3) {
      return -1;
    }
    o += (size_t)snprintf(out + o, sizeof(out) - o, "%s%s", o ? " " : "", t);
  }
  if (n == 0) {
    return -1;
  }
  strcpy(v, out);
  return 0;
}

static int v_hostname(char *v)
{
  size_t n = strlen(v);
  if (n < 1 || n > 63 || v[0] == '-' || v[n - 1] == '-') {
    return -1;
  }
  for (size_t i = 0; i < n; ++i) {
    if (!isalnum((unsigned char)v[i]) && v[i] != '-') {
      return -1;
    }
  }
  return 0;
}

static int v_ssid(char *v)
{
  size_t n = strlen(v);
  return n >= 1 && n <= 32 ? 0 : -1;
}

static int v_psk(char *v)
{
  size_t n = strlen(v);
  if (n == 64) {
    for (size_t i = 0; i < n; ++i) {
      if (!isxdigit((unsigned char)v[i])) {
        return -1;
      }
    }
    return 0;
  }
  if (n < 8 || n > 63) {
    return -1;
  }
  for (size_t i = 0; i < n; ++i) {
    if ((unsigned char)v[i] < 0x20 || (unsigned char)v[i] > 0x7e) {
      return -1;
    }
  }
  return 0;
}

static int v_country(char *v)
{
  if (strlen(v) != 2 || !isalpha((unsigned char)v[0]) || !isalpha((unsigned char)v[1])) {
    return -1;
  }
  v[0] = (char)toupper((unsigned char)v[0]);
  v[1] = (char)toupper((unsigned char)v[1]);
  return 0;
}

/* 40 MHz reference, measured: accept +-250 ppm */
static int v_xo(char *v)
{
  char *e;
  if (!isdigit((unsigned char)v[0])) {
    return -1;
  }
  long hz = strtol(v, &e, 10);
  return *e == '\0' && hz >= 39990000L && hz <= 40010000L ? 0 : -1;
}

static int in_list(char *v, const char *const *list)
{
  for (char *p = v; *p; ++p) {
    *p = (char)tolower((unsigned char)*p);
  }
  for (; *list; ++list) {
    if (strcmp(v, *list) == 0) {
      return 0;
    }
  }
  return -1;
}

static int v_role(char *v)
{
  static const char *const l[] = {"slave", "master", "light", NULL};
  return in_list(v, l);
}

static int v_usb_mode(char *v)
{
  static const char *const l[] = {"peripheral", "auto", "host", NULL};
  return in_list(v, l);
}

static int v_cpu_clock(char *v)
{
  static const char *const l[] = {"standard", "495_396", "950_600", "1100_750", NULL};
  return in_list(v, l);
}

static int v_yesno(char *v)
{
  static const char *const l[] = {"yes", "no", NULL};
  return in_list(v, l);
}

static int v_onoff(char *v)
{
  static const char *const l[] = {"on", "off", NULL};
  return in_list(v, l);
}

static int v_01(char *v)
{
  static const char *const l[] = {"0", "1", NULL};
  return in_list(v, l);
}

static int v_u32(char *v)
{
  char *end = NULL;
  if (v[0] == '\0') {
    return -1;
  }
  (void)strtoul(v, &end, 0);
  return (end != NULL && *end == '\0') ? 0 : -1;
}

static int v_usb_eth_mode(char *v)
{
  static const char *const l[] = {"rndis", "ncm", "ecm", NULL};
  return in_list(v, l);
}

/* ---- known keys ---- */

typedef struct {
  const char *name;
  const char *section;
  unsigned flags;
  valid_fn valid;
} keydef_t;

static const keydef_t keys[] = {
  {"hostname", "NETWORK", CFG_F_UBOOT, v_hostname},
  /* USB host controller tuning (Zynq ChipIdea), applied after the config loads */
  {"usb_txfill", "USB", 0, v_u32},
  {"usb_sbuscfg", "USB", 0, v_u32},
  {"usb_burstsize", "USB", 0, v_u32},
  {"usb_aic_dc_switch", "USB", 0, v_01},
  {"ipaddr", "NETWORK", CFG_F_UBOOT, v_ipv4},
  {"ipaddr_host", "NETWORK", CFG_F_UBOOT, v_ipv4},
  {"netmask", "NETWORK", CFG_F_UBOOT, v_netmask},
  {"dns", "NETWORK", 0, v_dns},
  {"ssid_wlan", "WLAN", 0, v_ssid},
  {"pwd_wlan", "WLAN", CFG_F_SECRET, v_psk},
  {"ipaddr_wlan", "WLAN", CFG_F_UBOOT, v_ipv4_prefix},
  {"gateway_wlan", "WLAN", 0, v_ipv4},
  {"country_wlan", "WLAN", CFG_F_UBOOT, v_country},
  {"ssid_wlan1", "WLAN", 0, v_ssid},
  {"pwd_wlan1", "WLAN", CFG_F_SECRET, v_psk},
  {"ssid_wlan2", "WLAN", 0, v_ssid},
  {"pwd_wlan2", "WLAN", CFG_F_SECRET, v_psk},
  {"ssid_wlan3", "WLAN", 0, v_ssid},
  {"pwd_wlan3", "WLAN", CFG_F_SECRET, v_psk},
  {"ssid_wlan4", "WLAN", 0, v_ssid},
  {"pwd_wlan4", "WLAN", CFG_F_SECRET, v_psk},
  {"ipaddr_eth", "USB_ETHERNET", CFG_F_UBOOT, v_ipv4_prefix},
  {"netmask_eth", "USB_ETHERNET", CFG_F_UBOOT, v_netmask},
  {"gateway_eth", "USB_ETHERNET", 0, v_ipv4},
  {"xo_correction", "SYSTEM", CFG_F_UBOOT, v_xo},
  {"udc_handle_suspend", "SYSTEM", CFG_F_UBOOT, v_01},
  {"usb_ethernet_mode", "SYSTEM", CFG_F_UBOOT, v_usb_eth_mode},
  {"operating_mode", "SYSTEM", CFG_F_UBOOT, v_role},
  {"usb_mode", "SYSTEM", CFG_F_UBOOT, v_usb_mode},
  {"cpu_clock", "SYSTEM", CFG_F_UBOOT, v_cpu_clock},
  {"cpu_clock_cooling_confirmed", "SYSTEM", CFG_F_UBOOT, v_yesno},
  {"bluetooth", "SYSTEM", CFG_F_UBOOT, v_onoff},
};
#define NKEYS (sizeof(keys) / sizeof(keys[0]))

/* [ACTIONS]: reset and export act once and are not stored; the others are stored as
 * action_<name> = 1 for their consumers (which clear them) */
static const char *const actions[] = {"reset", "export", "calibrate", "diagnostic_report", "dfu", NULL};
static const char *const sections[] = {"NETWORK", "WLAN", "USB_ETHERNET", "SYSTEM", "SSH", "ACTIONS", NULL};

static const keydef_t *find_key(const char *name)
{
  for (size_t i = 0; i < NKEYS; ++i) {
    if (strcmp(keys[i].name, name) == 0) {
      return &keys[i];
    }
  }
  return NULL;
}

unsigned cfg_key_flags(const char *key)
{
  const keydef_t *d = find_key(key);
  if (d != NULL) {
    return d->flags;
  }
  if (strncmp(key, "action_", 7) == 0) {
    return CFG_F_ACTION;
  }
  return cfg_is_secret(key) ? CFG_F_SECRET : 0;
}

int cfg_is_secret(const char *key)
{
  const keydef_t *d = find_key(key);
  if (d != NULL) {
    return (d->flags & CFG_F_SECRET) != 0;
  }
  if (strncmp(key, "ssh_authorized_key", 18) == 0) {
    return 0; /* public keys */
  }
  static const char *const pat[] = {"pwd", "psk", "pass", "secret", "token", "key", NULL};
  for (const char *const *p = pat; *p; ++p) {
    if (strstr(key, *p) != NULL) {
      return 1;
    }
  }
  return 0;
}

const char *cfg_display(const char *key, const char *value)
{
  if (value == NULL) {
    return "";
  }
  return value[0] != '\0' && cfg_is_secret(key) ? CFG_MASK : value;
}

void cfg_wifi_keys(int slot, char *ssid_key, char *psk_key, size_t cap)
{
  if (slot <= 0) {
    snprintf(ssid_key, cap, "ssid_wlan");
    snprintf(psk_key, cap, "pwd_wlan");
  } else {
    snprintf(ssid_key, cap, "ssid_wlan%d", slot);
    snprintf(psk_key, cap, "pwd_wlan%d", slot);
  }
}

int cfg_has_wifi(const kv_t *store)
{
  for (int s = 0; s < CFG_WIFI_SLOTS; ++s) {
    char sk[24], pk[24];
    cfg_wifi_keys(s, sk, pk, sizeof(sk));
    const char *v = kv_get(store, sk);
    if (v != NULL && v[0] != '\0') {
      return 1;
    }
  }
  return 0;
}

/* ---- parsing helpers ---- */

static char *trim(char *s)
{
  while (*s == ' ' || *s == '\t') {
    ++s;
  }
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
    s[--n] = '\0';
  }
  return s;
}

static char *unquote(char *s)
{
  size_t n = strlen(s);
  if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
    s[n - 1] = '\0';
    return s + 1;
  }
  return s;
}

static int needs_quotes(const char *v)
{
  size_t n = strlen(v);
  if (n == 0) {
    return 0;
  }
  if (v[0] == ' ' || v[0] == '\t' || v[n - 1] == ' ' || v[n - 1] == '\t') {
    return 1;
  }
  return n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0];
}

static void lower(char *s)
{
  for (; *s; ++s) {
    *s = (char)tolower((unsigned char)*s);
  }
}

static int valid_key_name(const char *k)
{
  if (*k == '\0' || strlen(k) > 63) {
    return 0;
  }
  for (; *k; ++k) {
    if (!isalnum((unsigned char)*k) && *k != '_' && *k != '-' && *k != '.') {
      return 0;
    }
  }
  return 1;
}

static int truthy(const char *v)
{
  return strcmp(v, "1") == 0 || strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0 ||
         strcasecmp(v, "on") == 0;
}

static int is_action(const char *k)
{
  for (const char *const *a = actions; *a; ++a) {
    if (strcmp(*a, k) == 0) {
      return 1;
    }
  }
  return 0;
}

/* map an input key (in a section) to its canonical name; returns 0 or -1 (not a known alias) */
static int canonical(const char *section, const char *key, char *out, size_t cap)
{
  int n;
  char rest[32];
  /* aliases valid everywhere: wifi_ssid, wifi_psk, wifiN_ssid, wifiN_psk */
  if (strcmp(key, "wifi_ssid") == 0) {
    snprintf(out, cap, "ssid_wlan");
    return 0;
  }
  if (strcmp(key, "wifi_psk") == 0) {
    snprintf(out, cap, "pwd_wlan");
    return 0;
  }
  if (sscanf(key, "wifi%d_%31s", &n, rest) == 2 && n >= 1 && n <= 4) {
    if (strcmp(rest, "ssid") == 0) {
      snprintf(out, cap, "ssid_wlan%d", n);
      return 0;
    }
    if (strcmp(rest, "psk") == 0) {
      snprintf(out, cap, "pwd_wlan%d", n);
      return 0;
    }
  }
  /* simple [wifi] section */
  if (strcasecmp(section, "wifi") == 0) {
    static const struct {
      const char *in, *out;
    } m[] = {{"ssid", "ssid_wlan"}, {"psk", "pwd_wlan"}, {"password", "pwd_wlan"},
             {"pwd", "pwd_wlan"}, {"country", "country_wlan"}, {"ipaddr", "ipaddr_wlan"},
             {"gateway", "gateway_wlan"}};
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); ++i) {
      if (strcmp(key, m[i].in) == 0) {
        snprintf(out, cap, "%s", m[i].out);
        return 0;
      }
    }
    char word[16];
    if (sscanf(key, "%15[a-z]%d", word, &n) == 2 && n >= 1 && n <= 4) {
      if (strcmp(word, "ssid") == 0) {
        snprintf(out, cap, "ssid_wlan%d", n);
        return 0;
      }
      if (strcmp(word, "psk") == 0 || strcmp(word, "password") == 0 || strcmp(word, "pwd") == 0) {
        snprintf(out, cap, "pwd_wlan%d", n);
        return 0;
      }
    }
  }
  return -1;
}

void cfg_update_init(cfg_update_t *u)
{
  memset(u, 0, sizeof(*u));
  kv_init(&u->set);
  kv_init(&u->actions);
}

void cfg_update_free(cfg_update_t *u)
{
  kv_free(&u->set);
  kv_free(&u->actions);
  for (size_t i = 0; i < u->n_ssh_keys; ++i) {
    free(u->ssh_keys[i]);
  }
  memset(u, 0, sizeof(*u));
}

static void nolog(void *arg, const char *fmt, ...)
{
  (void)arg;
  (void)fmt;
}

/* collapse whitespace of an OpenSSH public key line; 0 when it looks like one */
static int norm_ssh_key(const char *in, char *out, size_t cap)
{
  size_t o = 0;
  int fields = 0, sp = 1;
  for (const char *p = in; *p && o + 1 < cap; ++p) {
    if (*p == ' ' || *p == '\t') {
      sp = 1;
      continue;
    }
    if (sp) {
      if (o) {
        out[o++] = ' ';
      }
      ++fields;
      sp = 0;
    }
    out[o++] = *p;
  }
  out[o] = '\0';
  if (fields < 2 || o + 1 >= cap) {
    return -1;
  }
  if (strncmp(out, "ssh-", 4) != 0 && strncmp(out, "ecdsa-", 6) != 0 &&
      strncmp(out, "sk-", 3) != 0) {
    return -1;
  }
  return 0;
}

int cfg_parse(const char *text, size_t len, cfg_update_t *u, cfg_log_fn log, void *la)
{
  if (log == NULL) {
    log = nolog;
  }
  if (len > CFG_MAX_TEXT) {
    log(la, "config: file larger than %d bytes, ignored", CFG_MAX_TEXT);
    return 0;
  }
  char *buf = malloc(len + 1);
  if (buf == NULL) {
    return -1;
  }
  memcpy(buf, text, len);
  buf[len] = '\0';
  char *p = buf;
  if ((unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) {
    p += 3;
  }
  char section[32] = "";
  int accepted = 0, lineno = 0;
  while (p != NULL && *p) {
    char *nl = strchr(p, '\n');
    if (nl != NULL) {
      *nl = '\0';
    }
    char *line = trim(p);
    p = nl ? nl + 1 : NULL;
    ++lineno;
    if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
      continue;
    }
    if (line[0] == '[') {
      char *e = strchr(line, ']');
      if (e == NULL) {
        log(la, "config: line %d: bad section header", lineno);
        u->invalid++;
        continue;
      }
      *e = '\0';
      snprintf(section, sizeof(section), "%s", trim(line + 1));
      for (char *s = section; *s; ++s) {
        *s = (char)toupper((unsigned char)*s);
      }
      int known = strcmp(section, "WIFI") == 0;
      for (const char *const *s = sections; *s && !known; ++s) {
        known = strcmp(*s, section) == 0;
      }
      if (!known) {
        log(la, "config: line %d: unknown section [%s], its keys are kept as given", lineno,
            section);
      }
      continue;
    }
    char *eq = strchr(line, '=');
    if (eq == NULL) {
      log(la, "config: line %d: expected key = value", lineno);
      u->invalid++;
      continue;
    }
    *eq = '\0';
    char *key = trim(line);
    char *val = unquote(trim(eq + 1));
    lower(key);
    if (!valid_key_name(key)) {
      log(la, "config: line %d: bad key name", lineno);
      u->invalid++;
      continue;
    }

    if (strcmp(section, "ACTIONS") == 0 || (section[0] == '\0' && is_action(key))) {
      if (!is_action(key)) {
        log(la, "config: line %d: unknown action %s, ignored", lineno, key);
        u->unknown++;
        continue;
      }
      if (truthy(val)) {
        if (kv_set(&u->actions, key, "1") < 0) {
          free(buf);
          return -1;
        }
        ++accepted;
      }
      continue;
    }
    if (strcmp(key, "authorized_key") == 0 || strcmp(key, "ssh_authorized_key") == 0) {
      char norm[1024];
      if (norm_ssh_key(val, norm, sizeof(norm)) != 0) {
        log(la, "config: line %d: authorized_key is not an OpenSSH public key line", lineno);
        u->invalid++;
        continue;
      }
      if (u->n_ssh_keys >= CFG_SSH_KEYS) {
        log(la, "config: line %d: more than %d authorized keys, ignored", lineno, CFG_SSH_KEYS);
        u->invalid++;
        continue;
      }
      if ((u->ssh_keys[u->n_ssh_keys] = strdup(norm)) == NULL) {
        free(buf);
        return -1;
      }
      u->n_ssh_keys++;
      ++accepted;
      continue;
    }
    if ((strcmp(section, "SSH") == 0 && strcmp(key, "password") == 0) ||
        strcmp(key, "ssh_password") == 0) {
      log(la, "config: line %d: the SSH password is [dashboard.auth] password in nexus2.toml, "
              "ignored here", lineno);
      u->ignored++;
      continue;
    }

    char canon[64];
    if (canonical(section, key, canon, sizeof(canon)) != 0) {
      snprintf(canon, sizeof(canon), "%s", key);
    }
    const keydef_t *d = find_key(canon);
    int secret = d ? (d->flags & CFG_F_SECRET) != 0 : cfg_is_secret(canon);
    if (strcmp(val, CFG_MASK) == 0) {
      u->ignored++; /* exported placeholder: keep the stored value */
      continue;
    }
    char v[256];
    if (strlen(val) >= sizeof(v)) {
      log(la, "config: line %d: %s value too long, ignored", lineno, canon);
      u->invalid++;
      continue;
    }
    strcpy(v, val);
    if (d == NULL) {
      if (strncmp(canon, "action_", 7) == 0 || strncmp(canon, "ssh_authorized_key", 18) == 0) {
        log(la, "config: line %d: %s is managed by the firmware, ignored", lineno, canon);
        u->ignored++;
        continue;
      }
      log(la, "config: line %d: unknown key %s, kept", lineno, canon);
      u->unknown++;
    } else if (v[0] != '\0' && d->valid(v) != 0) {
      if (secret) {
        log(la, "config: line %d: invalid %s (value not shown), ignored", lineno, canon);
      } else {
        log(la, "config: line %d: invalid %s = %s, ignored", lineno, canon, v);
      }
      u->invalid++;
      continue;
    }
    /* "" would delete from the update list: "\x01" records "back to the default" */
    if (kv_set(&u->set, canon, v[0] != '\0' ? v : "\x01") < 0) {
      free(buf);
      return -1;
    }
    ++accepted;
  }
  free(buf);
  return accepted;
}

/* "\x01" in an update means: remove the key (back to the default) */
static const char *upd_value(const char *v)
{
  return strcmp(v, "\x01") == 0 ? "" : v;
}

int cfg_load_flat(const char *text, size_t len, kv_t *store)
{
  char *buf = malloc(len + 1);
  if (buf == NULL) {
    return -1;
  }
  memcpy(buf, text, len);
  buf[len] = '\0';
  int n = 0;
  for (char *p = buf; p != NULL && *p;) {
    char *nl = strchr(p, '\n');
    if (nl != NULL) {
      *nl = '\0';
    }
    char *line = trim(p);
    p = nl ? nl + 1 : NULL;
    if (line[0] == '\0' || line[0] == '#') {
      continue;
    }
    char *eq = strchr(line, '=');
    if (eq == NULL) {
      continue;
    }
    *eq = '\0';
    char *key = trim(line);
    char *val = unquote(trim(eq + 1));
    if (!valid_key_name(key) || val[0] == '\0') {
      continue;
    }
    if (kv_set(store, key, val) < 0) {
      free(buf);
      return -1;
    }
    ++n;
  }
  free(buf);
  return n;
}

char *cfg_store_text(const kv_t *store)
{
  sbuf_t b = {0};
  sb_add(&b, "# Nexus-BS RTOS configuration. Managed by the firmware: change it with the `config`\n"
             "# shell command or a config.txt on a USB drive, not by hand.\n");
  for (size_t i = 0; i < store->n; ++i) {
    const char *v = store->v[i];
    sb_add(&b, needs_quotes(v) ? "%s=\"%s\"\n" : "%s=%s\n", store->k[i], v);
  }
  return sb_done(&b);
}

static int same_ssh_key(const char *a, const char *b)
{
  /* type + base64 blob decide; the comment does not */
  const char *sa = strchr(a, ' '), *sb = strchr(b, ' ');
  if (sa == NULL || sb == NULL) {
    return strcmp(a, b) == 0;
  }
  const char *ea = strchr(sa + 1, ' '), *eb = strchr(sb + 1, ' ');
  size_t la = ea ? (size_t)(ea - a) : strlen(a);
  size_t lb = eb ? (size_t)(eb - b) : strlen(b);
  return la == lb && memcmp(a, b, la) == 0;
}

int cfg_merge(kv_t *store, const cfg_update_t *u, cfg_log_fn log, void *la)
{
  if (log == NULL) {
    log = nolog;
  }
  int changed = 0, r;
  for (size_t i = 0; i < u->set.n; ++i) {
    const char *k = u->set.k[i];
    const char *v = upd_value(u->set.v[i]);
    if ((r = kv_set(store, k, v)) < 0) {
      return -1;
    }
    changed += r;
    /* removing a network removes its passphrase too */
    if (v[0] == '\0' && strncmp(k, "ssid_wlan", 9) == 0) {
      char pk[24];
      snprintf(pk, sizeof(pk), "pwd_wlan%s", k + 9);
      if (kv_get(&u->set, pk) == NULL) {
        if ((r = kv_set(store, pk, NULL)) < 0) {
          return -1;
        }
        changed += r;
      }
    }
  }
  for (size_t i = 0; i < u->n_ssh_keys; ++i) {
    int have = 0, free_slot = 0;
    for (int s = 1; s <= CFG_SSH_KEYS; ++s) {
      char k[32];
      snprintf(k, sizeof(k), "ssh_authorized_key%d", s);
      const char *v = kv_get(store, k);
      if (v == NULL) {
        if (free_slot == 0) {
          free_slot = s;
        }
      } else if (same_ssh_key(v, u->ssh_keys[i])) {
        have = 1;
      }
    }
    if (have) {
      continue;
    }
    if (free_slot == 0) {
      log(la, "config: all %d SSH key slots are used, key not added", CFG_SSH_KEYS);
      continue;
    }
    char k[32];
    snprintf(k, sizeof(k), "ssh_authorized_key%d", free_slot);
    if (kv_set(store, k, u->ssh_keys[i]) < 0) {
      return -1;
    }
    ++changed;
  }
  for (size_t i = 0; i < u->actions.n; ++i) {
    const char *a = u->actions.k[i];
    if (strcmp(a, "reset") == 0 || strcmp(a, "export") == 0) {
      continue;
    }
    char k[48];
    snprintf(k, sizeof(k), "action_%s", a);
    if ((r = kv_set(store, k, "1")) < 0) {
      return -1;
    }
    changed += r;
  }
  return changed;
}

static void export_kv(sbuf_t *b, const char *name, const char *v)
{
  v = cfg_display(name, v);
  sb_add(b, needs_quotes(v) ? "%s = \"%s\"\n" : "%s = %s\n", name, v);
}

char *cfg_export_text(const kv_t *store)
{
  sbuf_t b = {0};
  sb_add(&b,
         "# Nexus-BS RTOS: current configuration of this Pluto, exported from the board.\n"
         "# Secrets are shown as %s: leave them so to keep the stored value. To apply changes,\n"
         "# edit, save as config.txt (top level or nexus/) and plug the drive in again.\n"
         "# Empty value = default. See rtos/config/README.md for every key.\n",
         CFG_MASK);
  for (const char *const *s = sections; *s; ++s) {
    sb_add(&b, "\n[%s]\n", *s);
    for (size_t i = 0; i < NKEYS; ++i) {
      if (strcmp(keys[i].section, *s) == 0) {
        export_kv(&b, keys[i].name, kv_get(store, keys[i].name));
      }
    }
    if (strcmp(*s, "SSH") == 0) {
      sb_add(&b, "# login password: [dashboard.auth] password in nexus2.toml\n");
      for (int k = 1; k <= CFG_SSH_KEYS; ++k) {
        char name[32];
        snprintf(name, sizeof(name), "ssh_authorized_key%d", k);
        const char *v = kv_get(store, name);
        if (v != NULL) {
          sb_add(&b, "authorized_key = %s\n", v);
        }
      }
    }
    if (strcmp(*s, "ACTIONS") == 0) {
      for (const char *const *a = actions; *a; ++a) {
        char name[48];
        snprintf(name, sizeof(name), "action_%s", *a);
        sb_add(&b, "%s = %s\n", *a, kv_get(store, name) ? "1" : "0");
      }
    }
  }
  int other = 0;
  for (size_t i = 0; i < store->n; ++i) {
    const char *k = store->k[i];
    if (find_key(k) != NULL || strncmp(k, "action_", 7) == 0 ||
        strncmp(k, "ssh_authorized_key", 18) == 0) {
      continue;
    }
    if (!other) {
      sb_add(&b, "\n# keys this firmware does not use (kept)\n[OTHER]\n");
      other = 1;
    }
    export_kv(&b, k, store->v[i]);
  }
  return sb_done(&b);
}

char *cfg_authorized_keys_text(const kv_t *store)
{
  sbuf_t b = {0};
  for (int k = 1; k <= CFG_SSH_KEYS; ++k) {
    char name[32];
    snprintf(name, sizeof(name), "ssh_authorized_key%d", k);
    const char *v = kv_get(store, name);
    if (v != NULL) {
      sb_add(&b, "%s\n", v);
    }
  }
  return sb_done(&b);
}

/* ---- CRC-32 ---- */

uint32_t cfg_crc32(uint32_t crc, const void *data, size_t len)
{
  static uint32_t table[256];
  static int init;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int j = 0; j < 8; ++j) {
        c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      }
      table[i] = c;
    }
    init = 1;
  }
  const uint8_t *p = data;
  crc = ~crc;
  while (len--) {
    crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  }
  return ~crc;
}

/* ---- U-Boot environment ---- */

int ubenv_parse(const uint8_t *img, size_t size, kv_t *env)
{
  if (size < 8) {
    return -1;
  }
  uint32_t stored = (uint32_t)img[0] | ((uint32_t)img[1] << 8) | ((uint32_t)img[2] << 16) |
                    ((uint32_t)img[3] << 24);
  if (cfg_crc32(0, img + 4, size - 4) != stored) {
    return -1;
  }
  kv_init(env);
  size_t i = 4;
  while (i < size && img[i] != '\0') {
    const char *s = (const char *)img + i;
    size_t n = strnlen(s, size - i);
    if (i + n >= size) {
      kv_free(env);
      return -1;
    }
    const char *eq = memchr(s, '=', n);
    if (eq != NULL && eq != s) {
      char key[128];
      size_t kl = (size_t)(eq - s);
      if (kl < sizeof(key)) {
        memcpy(key, s, kl);
        key[kl] = '\0';
        /* an empty value is kept as UBENV_EMPTY so the image is rebuilt unchanged */
        if (kv_set(env, key, eq[1] != '\0' ? eq + 1 : UBENV_EMPTY) < 0) {
          kv_free(env);
          return -1;
        }
      }
    }
    i += n + 1;
  }
  if (i >= size) {
    kv_free(env);
    return -1;
  }
  return 0;
}

int ubenv_build(const kv_t *env, uint8_t *img, size_t size, uint8_t fill)
{
  if (size < 8) {
    return -1;
  }
  memset(img, 0, size);
  size_t o = 4;
  for (size_t i = 0; i < env->n; ++i) {
    const char *v = strcmp(env->v[i], UBENV_EMPTY) == 0 ? "" : env->v[i];
    size_t kl = strlen(env->k[i]), vl = strlen(v);
    if (o + kl + 1 + vl + 1 + 1 > size) {
      return -1;
    }
    memcpy(img + o, env->k[i], kl);
    img[o + kl] = '=';
    memcpy(img + o + kl + 1, v, vl);
    o += kl + 1 + vl + 1; /* NUL already there */
  }
  /* list end: one more NUL (o), then the padding byte of the original image */
  if (o + 1 < size) {
    memset(img + o + 1, fill, size - o - 1);
  }
  uint32_t crc = cfg_crc32(0, img + 4, size - 4);
  img[0] = (uint8_t)crc;
  img[1] = (uint8_t)(crc >> 8);
  img[2] = (uint8_t)(crc >> 16);
  img[3] = (uint8_t)(crc >> 24);
  return 0;
}

int cfg_env_reflect(const cfg_update_t *u, kv_t *env)
{
  int changed = 0;
  for (size_t i = 0; i < u->set.n; ++i) {
    if ((cfg_key_flags(u->set.k[i]) & CFG_F_UBOOT) == 0) {
      continue;
    }
    int r = kv_set(env, u->set.k[i], upd_value(u->set.v[i]));
    if (r < 0) {
      return -1;
    }
    changed += r;
  }
  return changed;
}
