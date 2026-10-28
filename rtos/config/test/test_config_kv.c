/*
 * Host unit tests of rtos/app/config_kv.c (parser, merge, masking, export, CRC-32, U-Boot env).
 * Run: rtos/config/test/run.sh
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../../app/config_kv.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
static char logbuf[16384];

#define CHECK(c)                                                         \
  do {                                                                   \
    ++checks;                                                            \
    if (!(c)) {                                                          \
      ++fails;                                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                \
    }                                                                    \
  } while (0)
#define STREQ(a, b) CHECK((a) != NULL && strcmp((a), (b)) == 0)

static void capture(void *arg, const char *fmt, ...)
{
  (void)arg;
  size_t n = strlen(logbuf);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(logbuf + n, sizeof(logbuf) - n, fmt, ap);
  va_end(ap);
  n = strlen(logbuf);
  if (n + 1 < sizeof(logbuf)) {
    logbuf[n] = '\n';
    logbuf[n + 1] = '\0';
  }
}

static int parse(const char *text, cfg_update_t *u)
{
  cfg_update_init(u);
  logbuf[0] = '\0';
  return cfg_parse(text, strlen(text), u, capture, NULL);
}

static void test_linux_example(void)
{
  /* the Pluto Linux firmware's config.txt layout, CRLF line ends and a BOM */
  const char *t =
    "\xef\xbb\xbf# comment\r\n"
    "[NETWORK]\r\n"
    "hostname = pluto-ro\r\n"
    "ipaddr = 192.168.2.1\r\n"
    "netmask = 255.255.255.0\r\n"
    "\r\n"
    "[WLAN]\r\n"
    "ssid_wlan = MyNetwork\r\n"
    "pwd_wlan = s3cret pass#1\r\n"
    "ipaddr_wlan =\r\n"
    "country_wlan = de\r\n"
    "ssid_wlan2 = Backup Net\r\n"
    "pwd_wlan2 = backup-pass\r\n"
    "[USB_ETHERNET]\r\n"
    "ipaddr_eth = 10.0.0.5/24\r\n"
    "netmask_eth = 255.255.255.0\r\n"
    "gateway_eth = 10.0.0.1\r\n"
    "[SYSTEM]\r\n"
    "xo_correction = 39999331\r\n"
    "operating_mode = Master\r\n"
    "usb_mode = host\r\n"
    "cpu_clock = standard\r\n"
    "cpu_clock_cooling_confirmed = no\r\n"
    "bluetooth = off\r\n"
    "[ACTIONS]\r\n"
    "diagnostic_report = 0\r\n"
    "dfu = 0\r\n"
    "reset = 1\r\n"
    "calibrate = yes\r\n";
  cfg_update_t u;
  int n = parse(t, &u);
  CHECK(n == 20);
  STREQ(kv_get(&u.set, "hostname"), "pluto-ro");
  STREQ(kv_get(&u.set, "pwd_wlan"), "s3cret pass#1"); /* '#' inside a value is data */
  STREQ(kv_get(&u.set, "ipaddr_wlan"), "\x01");       /* empty = back to the default */
  STREQ(kv_get(&u.set, "country_wlan"), "DE");
  STREQ(kv_get(&u.set, "ssid_wlan2"), "Backup Net");
  STREQ(kv_get(&u.set, "ipaddr_eth"), "10.0.0.5/24");
  STREQ(kv_get(&u.set, "operating_mode"), "master");
  STREQ(kv_get(&u.set, "xo_correction"), "39999331");
  STREQ(kv_get(&u.actions, "reset"), "1");
  STREQ(kv_get(&u.actions, "calibrate"), "1");
  CHECK(kv_get(&u.actions, "dfu") == NULL);
  CHECK(u.invalid == 0 && u.unknown == 0);
  CHECK(strstr(logbuf, "s3cret") == NULL);

  kv_t store;
  kv_init(&store);
  CHECK(kv_set(&store, "ipaddr_wlan", "192.168.1.50/24") == 1);
  int ch = cfg_merge(&store, &u, capture, NULL);
  CHECK(ch > 0);
  CHECK(kv_get(&store, "ipaddr_wlan") == NULL); /* removed: DHCP again */
  STREQ(kv_get(&store, "action_calibrate"), "1");
  CHECK(kv_get(&store, "action_reset") == NULL); /* one-shot, not stored */
  /* merging the same file again changes nothing */
  CHECK(cfg_merge(&store, &u, capture, NULL) == 0);
  cfg_update_free(&u);
  kv_free(&store);
}

static void test_invalid_and_unknown(void)
{
  cfg_update_t u;
  int n = parse("[SYSTEM]\n"
                "operating_mode = boss\n"
                "xo_correction = 41000000\n"
                "usb_mode = auto\n"
                "[WLAN]\n"
                "pwd_wlan = short\n"
                "ssid_wlan = this-ssid-is-way-too-long-for-802-11\n"
                "[NETWORK]\n"
                "hostname = bad_name!\n"
                "netmask = 255.0.255.0\n"
                "dns = 1.1.1.1, 8.8.8.8\n"
                "my_custom = 42\n"
                "[STATION]\n"
                "foo = bar\n"
                "no equals sign here\n"
                "[ACTIONS]\n"
                "selfdestruct = 1\n",
                &u);
  CHECK(n == 4); /* usb_mode, dns, my_custom, foo */
  CHECK(u.invalid == 7);
  CHECK(u.unknown == 3);
  STREQ(kv_get(&u.set, "usb_mode"), "auto");
  STREQ(kv_get(&u.set, "dns"), "1.1.1.1 8.8.8.8");
  STREQ(kv_get(&u.set, "my_custom"), "42");
  STREQ(kv_get(&u.set, "foo"), "bar");
  CHECK(kv_get(&u.set, "operating_mode") == NULL);
  CHECK(kv_get(&u.set, "pwd_wlan") == NULL);
  CHECK(strstr(logbuf, "short") == NULL);          /* secret never logged */
  CHECK(strstr(logbuf, "value not shown") != NULL);
  CHECK(strstr(logbuf, "boss") != NULL);           /* non-secret shown */
  cfg_update_free(&u);
}

static void test_aliases_and_wifi_section(void)
{
  cfg_update_t u;
  int n = parse("wifi_ssid = Home\n"
                "wifi_psk = homepassword\n"
                "wifi3_ssid = Third\n"
                "wifi3_psk = 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "[wifi]\n"
                "ssid1 = Office\n"
                "psk1 = officepass\n"
                "country = ro\n"
                "ssid4 = Cafe\n"
                "psk4 =\n",
                &u);
  CHECK(n == 9);
  STREQ(kv_get(&u.set, "ssid_wlan"), "Home");
  STREQ(kv_get(&u.set, "pwd_wlan"), "homepassword");
  STREQ(kv_get(&u.set, "ssid_wlan3"), "Third");
  STREQ(kv_get(&u.set, "ssid_wlan1"), "Office");
  STREQ(kv_get(&u.set, "pwd_wlan1"), "officepass");
  STREQ(kv_get(&u.set, "country_wlan"), "RO");
  STREQ(kv_get(&u.set, "ssid_wlan4"), "Cafe");
  kv_t store;
  kv_init(&store);
  kv_set(&store, "pwd_wlan4", "oldpassword");
  cfg_merge(&store, &u, NULL, NULL);
  CHECK(kv_get(&store, "pwd_wlan4") == NULL); /* empty psk = open network */
  CHECK(cfg_has_wifi(&store));
  cfg_update_free(&u);

  /* removing a network removes its passphrase */
  parse("ssid_wlan1 =\n", &u);
  cfg_merge(&store, &u, NULL, NULL);
  CHECK(kv_get(&store, "ssid_wlan1") == NULL && kv_get(&store, "pwd_wlan1") == NULL);
  STREQ(kv_get(&store, "pwd_wlan"), "homepassword");
  cfg_update_free(&u);
  kv_free(&store);
}

static void test_masking_and_export_roundtrip(void)
{
  kv_t store;
  kv_init(&store);
  kv_set(&store, "ssid_wlan", "Home");
  kv_set(&store, "pwd_wlan", "homepassword");
  kv_set(&store, "hostname", "pluto");
  kv_set(&store, "ssh_authorized_key1", "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIExample op@laptop");
  kv_set(&store, "api_token", "abcdef");
  kv_set(&store, "action_calibrate", "1");
  STREQ(cfg_display("pwd_wlan", "x"), CFG_MASK);
  STREQ(cfg_display("pwd_wlan", ""), "");
  STREQ(cfg_display("api_token", "abcdef"), CFG_MASK);
  STREQ(cfg_display("ssh_authorized_key1", "ssh-ed25519 AAA"), "ssh-ed25519 AAA");
  STREQ(cfg_display("hostname", "pluto"), "pluto");

  char *t = cfg_export_text(&store);
  CHECK(t != NULL);
  CHECK(strstr(t, "homepassword") == NULL);
  CHECK(strstr(t, "abcdef") == NULL);
  CHECK(strstr(t, "pwd_wlan = " CFG_MASK) != NULL);
  CHECK(strstr(t, "[OTHER]\napi_token = " CFG_MASK) != NULL);
  CHECK(strstr(t, "authorized_key = ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIExample op@laptop") != NULL);
  CHECK(strstr(t, "calibrate = 1") != NULL);
  CHECK(strstr(t, "reset = 0") != NULL);

  /* the exported file dropped back unchanged changes nothing (masks keep the secrets) */
  cfg_update_t u;
  parse(t, &u);
  CHECK(u.invalid == 0);
  kv_t before;
  kv_copy(&before, &store);
  int ch = cfg_merge(&store, &u, capture, NULL);
  CHECK(ch == 0);
  STREQ(kv_get(&store, "pwd_wlan"), "homepassword");
  STREQ(kv_get(&store, "api_token"), "abcdef");
  CHECK(store.n == before.n);
  cfg_update_free(&u);
  free(t);
  kv_free(&before);
  kv_free(&store);
}

static void test_ssh(void)
{
  cfg_update_t u;
  int n = parse("[SSH]\n"
                "authorized_key = ssh-ed25519   AAAAkey1   alice@x\n"
                "authorized_key = ssh-rsa AAAAkey2\n"
                "authorized_key = not a key\n"
                "password = hunter22\n",
                &u);
  CHECK(n == 2);
  CHECK(u.n_ssh_keys == 2);
  CHECK(u.invalid == 1 && u.ignored == 1);
  CHECK(strstr(logbuf, "hunter22") == NULL);
  CHECK(strstr(logbuf, "nexus2.toml") != NULL);
  kv_t store;
  kv_init(&store);
  CHECK(cfg_merge(&store, &u, NULL, NULL) == 2);
  STREQ(kv_get(&store, "ssh_authorized_key1"), "ssh-ed25519 AAAAkey1 alice@x");
  STREQ(kv_get(&store, "ssh_authorized_key2"), "ssh-rsa AAAAkey2");
  CHECK(kv_get(&store, "ssh_password") == NULL);
  cfg_update_free(&u);
  /* same key with another comment is not added twice */
  parse("authorized_key = ssh-ed25519 AAAAkey1 alice@other\n", &u);
  CHECK(cfg_merge(&store, &u, NULL, NULL) == 0);
  char *ak = cfg_authorized_keys_text(&store);
  STREQ(ak, "ssh-ed25519 AAAAkey1 alice@x\nssh-rsa AAAAkey2\n");
  free(ak);
  cfg_update_free(&u);
  kv_free(&store);
}

static void test_store_roundtrip(void)
{
  kv_t s, r;
  kv_init(&s);
  kv_init(&r);
  kv_set(&s, "ssid_wlan", "Net With Spaces");
  kv_set(&s, "pwd_wlan", " leading and trailing ");
  kv_set(&s, "x", "\"quoted\"");
  kv_set(&s, "eq", "a=b=c");
  char *t = cfg_store_text(&s);
  CHECK(cfg_load_flat(t, strlen(t), &r) == 4);
  STREQ(kv_get(&r, "ssid_wlan"), "Net With Spaces");
  STREQ(kv_get(&r, "pwd_wlan"), " leading and trailing ");
  STREQ(kv_get(&r, "x"), "\"quoted\"");
  STREQ(kv_get(&r, "eq"), "a=b=c");
  free(t);
  kv_free(&s);
  kv_free(&r);
}

static void test_crc_and_uboot_env(void)
{
  CHECK(cfg_crc32(0, "123456789", 9) == 0xcbf43926u);
  enum { SZ = 0x20000 };
  uint8_t *img = calloc(1, SZ), *img2 = calloc(1, SZ);
  kv_t env, env2;
  kv_init(&env);
  kv_set(&env, "bootcmd", "run nexus_qspiboot");
  kv_set(&env, "fit_config", "config-2");
  kv_set(&env, "xo_correction", "40000000");
  kv_set(&env, "preboot", UBENV_EMPTY);
  CHECK(ubenv_build(&env, img, SZ, 0) == 0);
  /* layout: CRC LE, "k=v\0"..., "\0", zero padding */
  CHECK(memcmp(img + 4, "bootcmd=run nexus_qspiboot\0fit_config=config-2\0", 47) == 0);
  CHECK(ubenv_parse(img, SZ, &env2) == 0);
  STREQ(kv_get(&env2, "fit_config"), "config-2");
  STREQ(kv_get(&env2, "preboot"), UBENV_EMPTY);
  /* rebuild of an unchanged parse is bit-identical (empty values preserved) */
  CHECK(ubenv_build(&env2, img2, SZ, 0) == 0 && memcmp(img, img2, SZ) == 0);
  /* corrupted image refused */
  img2[100] ^= 1;
  kv_t bad;
  CHECK(ubenv_parse(img2, SZ, &bad) != 0);

  /* reflect: only U-Boot keys, empty = unset, unchanged = no write */
  cfg_update_t u;
  parse("[SYSTEM]\nxo_correction = 39999331\noperating_mode = light\n"
        "[WLAN]\nssid_wlan = Home\npwd_wlan = homepassword\n[NETWORK]\nhostname =\n",
        &u);
  kv_set(&env2, "hostname", "old");
  int ch = cfg_env_reflect(&u, &env2);
  CHECK(ch == 3);
  STREQ(kv_get(&env2, "xo_correction"), "39999331");
  STREQ(kv_get(&env2, "operating_mode"), "light");
  CHECK(kv_get(&env2, "hostname") == NULL);
  CHECK(kv_get(&env2, "ssid_wlan") == NULL && kv_get(&env2, "pwd_wlan") == NULL);
  STREQ(kv_get(&env2, "bootcmd"), "run nexus_qspiboot");
  CHECK(cfg_env_reflect(&u, &env2) == 0);
  cfg_update_free(&u);

  /* too large refused */
  kv_t big;
  kv_init(&big);
  char *v = malloc(SZ);
  memset(v, 'a', SZ - 1);
  v[SZ - 1] = '\0';
  kv_set(&big, "huge", v);
  CHECK(ubenv_build(&big, img2, SZ, 0) != 0);
  free(v);
  kv_free(&big);
  kv_free(&env);
  kv_free(&env2);
  free(img);
  free(img2);
}

static void test_real_env_if_present(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    printf("skip: no U-Boot env dump at %s\n", path);
    return;
  }
  enum { SZ = 0x20000 };
  uint8_t *img = malloc(SZ), *img2 = malloc(SZ);
  size_t got = fread(img, 1, SZ, f);
  fclose(f);
  kv_t env;
  CHECK(got == SZ);
  int rc = ubenv_parse(img, SZ, &env);
  CHECK(rc == 0);
  if (rc == 0) {
    printf("real env: %lu variables, fit_config=%s\n", (unsigned long)env.n,
           kv_get(&env, "fit_config") ? kv_get(&env, "fit_config") : "(none)");
    CHECK(ubenv_build(&env, img2, SZ, img[SZ - 1]) == 0);
    /* same order, same padding byte: normally bit-identical (stale bytes between the list end
     * and the padding of an older, longer list are not reproduced; the CRC stays valid) */
    size_t diff = 0;
    for (size_t i = 0; i < SZ; ++i) {
      diff += img[i] != img2[i];
    }
    /* (a vendor image with a duplicated variable is rebuilt with it once: same meaning) */
    printf("real env: rebuild differs in %lu bytes\n", (unsigned long)diff);
    kv_t again;
    CHECK(ubenv_parse(img2, SZ, &again) == 0);
    CHECK(again.n == env.n);
    kv_free(&again);
    kv_free(&env);
  }
  free(img);
  free(img2);
}

static void test_example_file(const char *path)
{
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  if (f == NULL) {
    return;
  }
  static char text[CFG_MAX_TEXT];
  size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = '\0';
  cfg_update_t u;
  int acc = parse(text, &u);
  printf("example: %d entries, %d invalid, %d unknown\n", acc, u.invalid, u.unknown);
  CHECK(acc > 20 && u.invalid == 0 && u.unknown == 0);
  STREQ(kv_get(&u.set, "operating_mode"), "master");
  CHECK(kv_get(&u.actions, "reset") == NULL); /* all actions 0 */
  cfg_update_free(&u);
}

int main(int argc, char **argv)
{
  test_linux_example();
  test_invalid_and_unknown();
  test_aliases_and_wifi_section();
  test_masking_and_export_roundtrip();
  test_ssh();
  test_store_roundtrip();
  test_crc_and_uboot_env();
  if (argc > 1) {
    test_example_file(argv[1]);
  }
  if (argc > 2) {
    test_real_env_if_present(argv[2]);
  }
  printf("%d checks, %d failed\n", checks, fails);
  return fails != 0;
}
