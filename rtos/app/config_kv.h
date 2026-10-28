/*
 * Portable part of the Nexus-BS RTOS configuration: key=value store, config.txt parser (Pluto
 * Linux firmware INI sections), validation, secret masking, export, CRC-32 and the U-Boot
 * environment image format. No RTEMS dependency: rtos/config/test builds it with the host cc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEXUS_CONFIG_KV_H
#define NEXUS_CONFIG_KV_H

#include <stddef.h>
#include <stdint.h>

#define CFG_MASK "********"         /* shown instead of a secret; never taken as a new value */
#define CFG_WIFI_SLOTS 5             /* ssid_wlan (slot 0) and ssid_wlan1..ssid_wlan4 */
#define CFG_SSH_KEYS 8               /* ssh_authorized_key1..8 */
#define CFG_MAX_TEXT (64 * 1024)     /* largest config.txt / nexus.conf accepted */

/* ordered key -> value list (insertion order is kept) */
typedef struct {
  char **k;
  char **v;
  size_t n, cap;
} kv_t;

void kv_init(kv_t *kv);
void kv_free(kv_t *kv);
const char *kv_get(const kv_t *kv, const char *key);
/* value NULL or "" removes the key; returns 1 when the store changed, 0 if not, -1 on ENOMEM */
int kv_set(kv_t *kv, const char *key, const char *value);
int kv_copy(kv_t *dst, const kv_t *src);

/* key flags */
#define CFG_F_SECRET 1u  /* masked on output */
#define CFG_F_UBOOT 2u   /* mirrored into the U-Boot environment (same name as on Linux) */
#define CFG_F_ACTION 4u  /* [ACTIONS] flag */

typedef void (*cfg_log_fn)(void *arg, const char *fmt, ...);

/* result of parsing one config.txt (or one `config set`) */
typedef struct {
  kv_t set;        /* canonical key -> value; "" = back to the default (key removed) */
  kv_t actions;    /* action name (reset, export, calibrate, ...) -> "1" */
  char *ssh_keys[CFG_SSH_KEYS];
  size_t n_ssh_keys;
  int unknown;     /* unknown keys (kept as given, warned) */
  int invalid;     /* rejected values (warned, not applied) */
  int ignored;     /* masked placeholders and dropped keys */
} cfg_update_t;

void cfg_update_init(cfg_update_t *u);
void cfg_update_free(cfg_update_t *u);

/* parse config.txt text: INI sections ([NETWORK] [WLAN] [USB_ETHERNET] [SYSTEM] [SSH] [ACTIONS]
 * [wifi]), `key = value`, '#'/';' comment lines, CRLF and a UTF-8 BOM tolerated.
 * Returns the number of accepted entries (settings + actions + ssh keys) or -1 on ENOMEM. */
int cfg_parse(const char *text, size_t len, cfg_update_t *u, cfg_log_fn log, void *log_arg);

/* parse the flat nexus.conf store (key=value, no sections, no validation) */
int cfg_load_flat(const char *text, size_t len, kv_t *store);

/* serialize the store as nexus.conf (malloc'd, NUL-terminated) */
char *cfg_store_text(const kv_t *store);

/* merge an update into the store; returns the number of keys whose value changed, -1 on ENOMEM.
 * SSH keys are added to the first free ssh_authorized_keyN unless already present. */
int cfg_merge(kv_t *store, const cfg_update_t *u, cfg_log_fn log, void *log_arg);

/* the store as a config.txt for the operator: all sections, secrets masked (malloc'd) */
char *cfg_export_text(const kv_t *store);

/* authorized_keys file content from ssh_authorized_key1..N (malloc'd, may be "") */
char *cfg_authorized_keys_text(const kv_t *store);

unsigned cfg_key_flags(const char *canonical_key);
int cfg_is_secret(const char *key);
/* value for display: CFG_MASK for a non-empty secret */
const char *cfg_display(const char *key, const char *value);

/* any Wi-Fi network configured? */
int cfg_has_wifi(const kv_t *store);
/* slot 0..4 -> "ssid_wlan" / "ssid_wlanN" and "pwd_wlan" / "pwd_wlanN" */
void cfg_wifi_keys(int slot, char *ssid_key, char *psk_key, size_t cap);

/* CRC-32 (IEEE 802.3, the zlib/U-Boot one) */
uint32_t cfg_crc32(uint32_t crc, const void *data, size_t len);

/* a variable present with an empty value ("name=") in a parsed U-Boot environment */
#define UBENV_EMPTY "\x02"

/* U-Boot environment image (non-redundant: CRC32 LE + NUL separated var=value, double NUL) */
int ubenv_parse(const uint8_t *img, size_t size, kv_t *env);   /* 0 ok, -1 CRC/format */
/* fill: padding after the list (mkenvimage writes 0xff, U-Boot saveenv 0x00): pass the last
 * byte of the image being replaced */
int ubenv_build(const kv_t *env, uint8_t *img, size_t size, uint8_t fill); /* 0 ok, -1 too large */
/* reflect the U-Boot keys of an update into env; returns the number of changed variables */
int cfg_env_reflect(const cfg_update_t *u, kv_t *env);

#endif
