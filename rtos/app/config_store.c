/*
 * Nexus-BS RTOS persistent configuration (see config_store.h, rtos/config/README.md).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "config_store.h"
#include "config_kv.h"
#include "qspi_nor.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <rtems.h>
#include <rtems/bdbuf.h>
#include <rtems/dosfs.h>
#include <rtems/jffs2.h>
#include <rtems/libio.h>
#include <rtems/media.h>
#include <rtems/shell.h>

#if defined(__has_include)
#if __has_include("nexus_wifi_defaults.h")
#include "nexus_wifi_defaults.h" /* generated in the build directory (rtos/config/gen-wifi-defaults.sh) */
#endif
#endif
#ifndef NEXUS_WIFI_DEFAULT_SSID_LEN
#define NEXUS_WIFI_DEFAULT_SSID_LEN 0
#define NEXUS_WIFI_DEFAULT_PSK_LEN 0
static const unsigned char nexus_wifi_default_ssid[1] = {0};
static const unsigned char nexus_wifi_default_psk[1] = {0};
#endif
#define NEXUS_WIFI_DEFAULT_XOR 0x5a

#define CONF_TMP NEXUS_CONFIG_FILE ".tmp"
#define MARKER NEXUS_DATA_DIR "/.nexus-rtos"
#define SSH_DIR NEXUS_DATA_DIR "/ssh"
#define AUTH_KEYS SSH_DIR "/authorized_keys"
#define ENV_BAK NEXUS_DATA_DIR "/uboot-env.bak"
#define ENV_ORIG NEXUS_DATA_DIR "/uboot-env.orig"
#define STATION_TOML NEXUS_STATION_DIR "/nexus2.toml"
#define STATION_MAX (256 * 1024)
#define MEDIA_PRIO 150
#define MEDIA_STACK (64 * 1024)

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static kv_t store;
static int data_ok;   /* /data mounted and writable */
static int flash_ok;  /* QSPI layer usable */
static char *last_drive;
/* nexus_config_init() finished: the media listener holds drive drop-ins until then, because the
 * media server is started before /data is mounted (nexus_config_early) */
static pthread_mutex_t ready_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_cv = PTHREAD_COND_INITIALIZER;
static int config_ready;
static void (*callbacks[8])(void);
static int n_callbacks;

/* ---- console log (never with secret values: callers mask) ---- */

static void logf_cb(void *arg, const char *fmt, ...)
{
  (void)arg;
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
}

/* ---- interned values: pointers handed out stay valid forever ---- */

typedef struct intern {
  struct intern *next;
  char *s;
} intern_t;
static intern_t *interns;

static const char *intern(const char *s)
{
  if (s == NULL) {
    return NULL;
  }
  for (intern_t *i = interns; i != NULL; i = i->next) {
    if (strcmp(i->s, s) == 0) {
      return i->s;
    }
  }
  intern_t *n = malloc(sizeof(*n));
  char *c = strdup(s);
  if (n == NULL || c == NULL) {
    free(n);
    free(c);
    return NULL;
  }
  n->s = c;
  n->next = interns;
  interns = n;
  return c;
}

/* ---- small file helpers ---- */

static int read_file(const char *path, char **out, size_t *len, size_t max)
{
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 0 || (size_t)st.st_size > max) {
    close(fd);
    return -2;
  }
  char *b = malloc((size_t)st.st_size + 1);
  if (b == NULL) {
    close(fd);
    return -1;
  }
  size_t got = 0;
  while (got < (size_t)st.st_size) {
    ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
    if (r <= 0) {
      break;
    }
    got += (size_t)r;
  }
  close(fd);
  if (got != (size_t)st.st_size) {
    free(b);
    return -1;
  }
  b[got] = '\0';
  *out = b;
  *len = got;
  return 0;
}

static int write_all(int fd, const void *p, size_t n)
{
  const char *c = p;
  while (n) {
    ssize_t w = write(fd, c, n);
    if (w <= 0) {
      return -1;
    }
    c += w;
    n -= (size_t)w;
  }
  return 0;
}

/* write tmp, fsync, rename over path */
static int write_atomic(const char *path, const void *data, size_t len, mode_t mode)
{
  char tmp[160];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, mode);
  if (fd < 0) {
    return -1;
  }
  int rc = write_all(fd, data, len);
  if (rc == 0) {
    /* RTEMS JFFS2 has no fsync (EINVAL): its NOR writes are synchronous already */
    rc = fsync(fd);
    if (rc != 0 && errno == EINVAL) {
      rc = 0;
    }
  }
  if (close(fd) != 0) {
    rc = -1;
  }
  if (rc == 0) {
    rc = rename(tmp, path);
    if (rc != 0 && errno == EEXIST) {
      /* RTEMS JFFS2 does not rename over an existing file: keep the old one as .old until
       * the new one is in place, so an interruption always leaves a valid file (recovered by
       * recover_swap() at the next boot). Not .bak: nexus2.toml.bak is the station rollback
       * copy, which this swap would otherwise delete right after install_station() wrote it. */
      char bak[168];
      snprintf(bak, sizeof(bak), "%s.old", path);
      (void)unlink(bak);
      rc = rename(path, bak);
      if (rc == 0) {
        rc = rename(tmp, path);
        if (rc == 0) {
          (void)unlink(bak);
        } else {
          (void)rename(bak, path);
        }
      }
    }
  }
  if (rc != 0) {
    unlink(tmp);
  }
  return rc;
}

static int file_exists(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

/* finish or undo a write_atomic() swap that a reset interrupted: <path>.old without <path> is
 * the previous valid file; with <path> present it is stale. A leftover .tmp is never valid. */
static void recover_swap(const char *path)
{
  char old[168], tmp[168];
  snprintf(old, sizeof(old), "%s.old", path);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  (void)unlink(tmp);
  if (!file_exists(old)) {
    return;
  }
  if (!file_exists(path)) {
    if (rename(old, path) == 0) {
      printf("config: %s restored from an interrupted update\n", path);
    }
  } else {
    (void)unlink(old);
  }
}

/* ---- JFFS2 over the QSPI nvmfs region ---- */

/* The JFFS2 file system type is registered at run time (no CONFIGURE_FILESYSTEM_JFFS2), so the
 * configuration confdefs would provide comes from here. */
const rtems_jffs2_config jffs2_config = {
  .delayed_write_priority = RTEMS_JFFS2_DELAYED_WRITE_TASK_PRIORITY_DEFAULT,
};

static int fc_read(rtems_jffs2_flash_control *fc, uint32_t off, unsigned char *buf, size_t n)
{
  (void)fc;
  if (off > QSPI_NOR_DATA_SIZE || n > QSPI_NOR_DATA_SIZE - off) {
    return -EIO;
  }
  return qspi_nor_read(QSPI_NOR_DATA_OFF + off, buf, n) == 0 ? 0 : -EIO;
}

static int fc_write(rtems_jffs2_flash_control *fc, uint32_t off, const unsigned char *buf,
                    size_t n)
{
  (void)fc;
  if (off > QSPI_NOR_DATA_SIZE || n > QSPI_NOR_DATA_SIZE - off) {
    return -EIO;
  }
  return qspi_nor_write(QSPI_NOR_DATA_OFF + off, buf, n) == 0 ? 0 : -EIO;
}

static int fc_erase(rtems_jffs2_flash_control *fc, uint32_t off)
{
  (void)fc;
  if (off >= QSPI_NOR_DATA_SIZE || (off % QSPI_NOR_SECTOR) != 0) {
    return -EIO;
  }
  return qspi_nor_erase_sector(QSPI_NOR_DATA_OFF + off) == 0 ? 0 : -EIO;
}

/* NOR: write_size 0 (no write buffer), no bad blocks, no OOB */
static rtems_jffs2_flash_control flash_control = {
  .block_size = QSPI_NOR_SECTOR,
  .flash_size = QSPI_NOR_DATA_SIZE,
  .write_size = 0,
  .read = fc_read,
  .write = fc_write,
  .erase = fc_erase,
  .device_identifier = 0x4e584346, /* any constant unique among mounted file systems */
};

static rtems_jffs2_compressor_control compressor = {
  .compress = rtems_jffs2_compressor_rtime_compress,
  .decompress = rtems_jffs2_compressor_rtime_decompress,
};

static rtems_jffs2_mount_data mount_data = {
  .flash_control = &flash_control,
  .compressor_control = &compressor,
};

static int mount_data_fs(void)
{
  flash_control.flash_size = QSPI_NOR_DATA_SIZE;
  return mount(NULL, NEXUS_DATA_DIR, RTEMS_FILESYSTEM_TYPE_JFFS2, RTEMS_FILESYSTEM_READ_WRITE,
               &mount_data);
}

static int erase_data_region(void)
{
  for (uint32_t off = 0; off < QSPI_NOR_DATA_SIZE; off += QSPI_NOR_SECTOR) {
    if (qspi_nor_erase_sector(QSPI_NOR_DATA_OFF + off) != 0) {
      return -1;
    }
  }
  return 0;
}

static void data_report(const char *how)
{
  struct statvfs sv;
  unsigned long free_kib = 0;
  if (statvfs(NEXUS_DATA_DIR, &sv) == 0) {
    free_kib = (unsigned long)((unsigned long long)sv.f_bavail * sv.f_frsize / 1024);
  }
  printf("config: /data %s (jffs2 on qspi 0x%06x+0x%05x, %lu KiB free)\n", how,
         QSPI_NOR_DATA_OFF, QSPI_NOR_DATA_SIZE, free_kib);
}

static int data_mount(void)
{
  (void)rtems_filesystem_register(RTEMS_FILESYSTEM_TYPE_JFFS2, rtems_jffs2_initialize);
  (void)mkdir(NEXUS_DATA_DIR, 0755);
  if (mount_data_fs() == 0) {
    if (!file_exists(MARKER)) {
      /* first RTEMS boot on this region: it held the Linux firmware's jffs2 (backed up) */
      printf("config: /data has no RTEMS marker (earlier contents kept; `config format yes` "
             "wipes them)\n");
      (void)write_atomic(MARKER, "nexus-rtos\n", 11, 0644);
    }
    data_report("mounted");
    return 0;
  }
  int mount_errno = errno;
  if (mount_errno == EIO || mount_errno == ENOMEM) {
    /* a flash read error or a memory shortage says nothing about the contents: never wipe
     * the configuration for it (`config format yes` does that on request) */
    printf("config: /data does not mount (errno %d), configuration in RAM only\n", mount_errno);
    return -1;
  }
  printf("config: /data does not mount (errno %d), formatting the nvmfs region\n", mount_errno);
  if (erase_data_region() != 0) {
    printf("config: erase failed, configuration kept in RAM only\n");
    return -1;
  }
  if (mount_data_fs() != 0) {
    printf("config: /data does not mount after the erase (errno %d), RAM only\n", errno);
    return -1;
  }
  (void)write_atomic(MARKER, "nexus-rtos\n", 11, 0644);
  data_report("formatted and mounted");
  return 0;
}

/* ---- persistence (mtx held) ---- */

static int save_locked(void)
{
  if (!data_ok) {
    return -1;
  }
  char *t = cfg_store_text(&store);
  if (t == NULL) {
    return -1;
  }
  int rc = write_atomic(NEXUS_CONFIG_FILE, t, strlen(t), 0600);
  free(t);
  if (rc != 0) {
    printf("config: writing %s failed (errno %d)\n", NEXUS_CONFIG_FILE, errno);
  }
  return rc;
}

static int save_auth_keys_locked(void)
{
  if (!data_ok) {
    return -1;
  }
  char *t = cfg_authorized_keys_text(&store);
  if (t == NULL) {
    return -1;
  }
  (void)mkdir(SSH_DIR, 0700);
  int rc = write_atomic(AUTH_KEYS, t, strlen(t), 0600);
  free(t);
  return rc;
}

static void notify(void)
{
  void (*cbs[8])(void);
  int n;
  pthread_mutex_lock(&mtx);
  n = n_callbacks;
  memcpy(cbs, callbacks, sizeof(cbs));
  pthread_mutex_unlock(&mtx);
  for (int i = 0; i < n; ++i) {
    cbs[i]();
  }
}

/* ---- U-Boot environment mirroring ---- */

static int update_has_uboot_keys(const cfg_update_t *u)
{
  for (size_t i = 0; i < u->set.n; ++i) {
    if (cfg_key_flags(u->set.k[i]) & CFG_F_UBOOT) {
      return 1;
    }
  }
  return 0;
}

static int env_flash(const uint8_t *img)
{
  for (uint32_t off = 0; off < QSPI_NOR_ENV_SIZE; off += QSPI_NOR_SECTOR) {
    if (qspi_nor_erase_sector(QSPI_NOR_ENV_OFF + off) != 0) {
      return -1;
    }
  }
  return qspi_nor_write(QSPI_NOR_ENV_OFF, img, QSPI_NOR_ENV_SIZE);
}

/* 0 = written or nothing to do, -1 = not written (values stay in /data) */
static int env_update(const cfg_update_t *u)
{
  if (!update_has_uboot_keys(u)) {
    return 0;
  }
  if (!flash_ok || !data_ok) {
    printf("config: U-Boot environment not updated (no flash or no /data for the backup)\n");
    return -1;
  }
  uint8_t *old = malloc(QSPI_NOR_ENV_SIZE);
  uint8_t *img = malloc(QSPI_NOR_ENV_SIZE);
  kv_t env;
  int rc = -1;
  kv_init(&env);
  if (old == NULL || img == NULL) {
    goto out;
  }
  if (qspi_nor_read(QSPI_NOR_ENV_OFF, old, QSPI_NOR_ENV_SIZE) != 0 ||
      ubenv_parse(old, QSPI_NOR_ENV_SIZE, &env) != 0) {
    printf("config: U-Boot environment unreadable or CRC bad: not changed (values kept in "
           "/data)\n");
    goto out;
  }
  int changed = cfg_env_reflect(u, &env);
  if (changed <= 0) {
    rc = changed == 0 ? 0 : -1;
    goto out;
  }
  if (ubenv_build(&env, img, QSPI_NOR_ENV_SIZE, old[QSPI_NOR_ENV_SIZE - 1]) != 0) {
    printf("config: U-Boot environment would overflow 128 KiB: not changed\n");
    goto out;
  }
  if (!file_exists(ENV_ORIG) && write_atomic(ENV_ORIG, old, QSPI_NOR_ENV_SIZE, 0600) != 0) {
    printf("config: cannot save %s: U-Boot environment not changed\n", ENV_ORIG);
    goto out;
  }
  if (write_atomic(ENV_BAK, old, QSPI_NOR_ENV_SIZE, 0600) != 0) {
    printf("config: cannot save %s: U-Boot environment not changed\n", ENV_BAK);
    goto out;
  }
  if (env_flash(img) != 0) {
    printf("config: U-Boot environment write FAILED, restoring the previous image\n");
    if (env_flash(old) != 0) {
      printf("config: RESTORE FAILED: U-Boot will use its built-in environment; restore %s "
             "(uboot-env.dfu) over DFU\n", ENV_BAK);
    }
    goto out;
  }
  printf("config: U-Boot environment updated (%d variables), previous image in %s\n", changed,
         ENV_BAK);
  rc = 0;
out:
  kv_free(&env);
  free(old);
  free(img);
  return rc;
}

/* ---- applying an update ---- */

/* merge, persist, mirror; *changed = keys changed. 0 = persisted (or nothing to persist) */
static int apply_update(const cfg_update_t *u, int *changed)
{
  int ssh_before = 0, ssh_after = 0, rc = 0;
  pthread_mutex_lock(&mtx);
  for (size_t i = 0; i < store.n; ++i) {
    ssh_before += strncmp(store.k[i], "ssh_authorized_key", 18) == 0;
  }
  char *before = cfg_authorized_keys_text(&store);
  int n = cfg_merge(&store, u, logf_cb, NULL);
  char *after = cfg_authorized_keys_text(&store);
  for (size_t i = 0; i < store.n; ++i) {
    ssh_after += strncmp(store.k[i], "ssh_authorized_key", 18) == 0;
  }
  if (n < 0) {
    rc = -1;
  } else if (n > 0) {
    rc = save_locked();
    if (rc == 0 && (ssh_before != ssh_after || before == NULL || after == NULL ||
                    strcmp(before, after) != 0)) {
      (void)save_auth_keys_locked();
    }
  }
  free(before);
  free(after);
  pthread_mutex_unlock(&mtx);
  if (changed != NULL) {
    *changed = n < 0 ? 0 : n;
  }
  if (rc == 0) {
    (void)env_update(u); /* failure is logged; the values are in /data either way */
  }
  if (n > 0) {
    notify();
  }
  return rc;
}

/* ---- public API ---- */

const char *nexus_config_get(const char *key)
{
  if (key == NULL) {
    return NULL;
  }
  /* the input aliases of the Wi-Fi keys work here too */
  char canon[32];
  int slot;
  char what[8];
  if (strcmp(key, "wifi_ssid") == 0) {
    key = "ssid_wlan";
  } else if (strcmp(key, "wifi_psk") == 0) {
    key = "pwd_wlan";
  } else if (sscanf(key, "wifi%d_%7s", &slot, what) == 2 && slot >= 1 && slot <= 4) {
    snprintf(canon, sizeof(canon), "%s_wlan%d", strcmp(what, "psk") == 0 ? "pwd" : "ssid", slot);
    key = canon;
  }
  pthread_mutex_lock(&mtx);
  const char *v = intern(kv_get(&store, key));
  pthread_mutex_unlock(&mtx);
  return v;
}

int nexus_config_set(const char *key, const char *value)
{
  if (key == NULL || strchr(key, '\n') != NULL || (value && strchr(value, '\n') != NULL)) {
    return -1;
  }
  if (strncmp(key, "action_", 7) == 0 && (value == NULL || value[0] == '\0')) {
    /* consumers clear their [ACTIONS] flag */
    pthread_mutex_lock(&mtx);
    int r = kv_set(&store, key, NULL);
    int rc = r > 0 ? save_locked() : (r < 0 ? -1 : 0);
    pthread_mutex_unlock(&mtx);
    return rc;
  }
  size_t len = strlen(key) + (value ? strlen(value) : 0) + 8;
  char *line = malloc(len);
  if (line == NULL) {
    return -1;
  }
  snprintf(line, len, "%s = %s\n", key, value ? value : "");
  cfg_update_t u;
  cfg_update_init(&u);
  int acc = cfg_parse(line, strlen(line), &u, logf_cb, NULL);
  free(line);
  int rc = -1;
  if (acc > 0) {
    rc = apply_update(&u, NULL);
  }
  cfg_update_free(&u);
  return rc;
}

int nexus_config_wifi(int index, const char **ssid, const char **psk)
{
  if (index < 0 || index >= CFG_WIFI_SLOTS) {
    return -1;
  }
  char sk[24], pk[24];
  cfg_wifi_keys(index, sk, pk, sizeof(sk));
  pthread_mutex_lock(&mtx);
  const char *s = kv_get(&store, sk);
  const char *p = kv_get(&store, pk);
  const char *is = s ? intern(s) : NULL;
  const char *ip = intern(p ? p : "");
  pthread_mutex_unlock(&mtx);
  if (is == NULL) {
    return -1;
  }
  if (ssid) {
    *ssid = is;
  }
  if (psk) {
    *psk = ip;
  }
  return 0;
}

void nexus_config_on_change(void (*cb)(void))
{
  pthread_mutex_lock(&mtx);
  if (cb != NULL && n_callbacks < (int)(sizeof(callbacks) / sizeof(callbacks[0]))) {
    callbacks[n_callbacks++] = cb;
  }
  pthread_mutex_unlock(&mtx);
}

const char *nexus_config_station_dir(void)
{
  return NEXUS_STATION_DIR;
}

int nexus_config_station_rollback(void)
{
  if (!data_ok || !file_exists(STATION_TOML ".bak")) {
    printf("config: no %s.bak to restore\n", STATION_TOML);
    return -1;
  }
  (void)unlink(STATION_TOML); /* RTEMS JFFS2 rename does not replace an existing file */
  if (rename(STATION_TOML ".bak", STATION_TOML) != 0) {
    printf("config: station rollback failed (errno %d)\n", errno);
    return -1;
  }
  printf("config: station nexus2.toml restored from nexus2.toml.bak\n");
  return 0;
}

int nexus_config_export(const char *dir)
{
  char path[256];
  pthread_mutex_lock(&mtx);
  if (dir == NULL) {
    dir = last_drive;
  }
  if (dir == NULL) {
    pthread_mutex_unlock(&mtx);
    printf("config: no USB drive mounted for the export\n");
    return -1;
  }
  snprintf(path, sizeof(path), "%s/config-current.txt", dir);
  char *t = cfg_export_text(&store);
  pthread_mutex_unlock(&mtx);
  if (t == NULL) {
    return -1;
  }
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  int rc = fd < 0 ? -1 : write_all(fd, t, strlen(t));
  if (fd >= 0) {
    if (rc == 0) {
      rc = fsync(fd);
      if (rc != 0 && errno == EINVAL) {
        rc = 0; /* RTEMS JFFS2: no fsync, writes are synchronous */
      }
    }
    close(fd);
  }
  free(t);
  printf(rc == 0 ? "config: exported to %s (secrets masked)\n" : "config: export to %s failed\n",
         path);
  return rc;
}

/* ---- USB drive drop-in ---- */

/* case-insensitive lookup of name in dir; result in out */
static int find_ci(const char *dir, const char *name, char *out, size_t cap, int want_dir)
{
  DIR *d = opendir(dir);
  if (d == NULL) {
    return -1;
  }
  int found = -1;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcasecmp(e->d_name, name) != 0) {
      continue;
    }
    snprintf(out, cap, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(out, &st) == 0 && (S_ISDIR(st.st_mode) != 0) == (want_dir != 0)) {
      found = 0;
      break;
    }
  }
  closedir(d);
  return found;
}

/* <root>/<name> or <root>/nexus/<name> */
static int find_dropin(const char *root, const char *name, char *out, size_t cap)
{
  char sub[256];
  if (find_ci(root, name, out, cap, 0) == 0) {
    return 0;
  }
  if (find_ci(root, "nexus", sub, sizeof(sub), 1) == 0 && find_ci(sub, name, out, cap, 0) == 0) {
    return 0;
  }
  return -1;
}

static void sync_dir_of(const char *path)
{
  char dir[256];
  snprintf(dir, sizeof(dir), "%s", path);
  char *s = strrchr(dir, '/');
  if (s != NULL && s != dir) {
    *s = '\0';
  }
  int fd = open(dir, O_RDONLY);
  if (fd >= 0) {
    (void)fsync(fd); /* dosfs: syncs the whole volume */
    close(fd);
  }
}

static void remove_from_drive(const char *path)
{
  if (unlink(path) != 0) {
    printf("config: could not remove %s (errno %d)\n", path, errno);
  }
  sync_dir_of(path);
}

static int install_station(const char *path)
{
  char *t = NULL;
  size_t len = 0;
  int r = read_file(path, &t, &len, STATION_MAX);
  if (r != 0) {
    printf("config: %s %s, not installed (kept on the drive)\n", path,
           r == -2 ? "too large" : "unreadable");
    return -1;
  }
  if (!data_ok) {
    printf("config: no /data, %s not installed (kept on the drive)\n", path);
    free(t);
    return -1;
  }
  (void)mkdir(NEXUS_STATION_DIR, 0755);
  int had = file_exists(STATION_TOML);
  if (had) {
    char *old = NULL;
    size_t olen = 0;
    if (read_file(STATION_TOML, &old, &olen, STATION_MAX) != 0 ||
        write_atomic(STATION_TOML ".bak", old, olen, 0600) != 0) {
      printf("config: cannot keep the previous nexus2.toml, %s not installed\n", path);
      free(old);
      free(t);
      return -1;
    }
    free(old);
  }
  if (write_atomic(STATION_TOML, t, len, 0600) != 0) {
    printf("config: writing %s failed, %s kept on the drive\n", STATION_TOML, path);
    free(t);
    return -1;
  }
  free(t);
  remove_from_drive(path);
  printf("config: station nexus2.toml installed (%lu bytes) from %s%s, file removed\n",
         (unsigned long)len, path, had ? ", previous kept as nexus2.toml.bak" : "");
  return 0;
}

/* returns the actions of the file (for reset/export) through reset and export_ */
static int apply_config_file(const char *path, int remove_after, int *reset, int *export_)
{
  char *t = NULL;
  size_t len = 0;
  int r = read_file(path, &t, &len, CFG_MAX_TEXT);
  if (r != 0) {
    printf("config: %s %s, not applied\n", path, r == -2 ? "too large" : "unreadable");
    return -1;
  }
  cfg_update_t u;
  cfg_update_init(&u);
  int acc = cfg_parse(t, len, &u, logf_cb, NULL);
  free(t);
  if (acc < 0) {
    cfg_update_free(&u);
    return -1;
  }
  int changed = 0;
  int rc = apply_update(&u, &changed);
  if (reset) {
    *reset = kv_get(&u.actions, "reset") != NULL;
  }
  if (export_) {
    *export_ = kv_get(&u.actions, "export") != NULL;
  }
  if (rc != 0) {
    printf("config: %s not applied (storage error), file kept\n", path);
  } else if (remove_after) {
    remove_from_drive(path);
    printf("config: applied %d keys from %s, file removed (%d changed", acc, path, changed);
    if (u.unknown || u.invalid || u.ignored) {
      printf(", %d unknown kept, %d invalid, %d ignored", u.unknown, u.invalid, u.ignored);
    }
    printf(")\n");
  } else {
    printf("config: applied %d keys from %s (%d changed)\n", acc, path, changed);
  }
  cfg_update_free(&u);
  return rc;
}

static void process_drive(const char *root)
{
  char path[256];
  int reset = 0, exp = 0;
  if (find_dropin(root, "nexus2.toml", path, sizeof(path)) == 0) {
    (void)install_station(path);
  }
  if (find_dropin(root, "config.txt", path, sizeof(path)) == 0) {
    (void)apply_config_file(path, 1, &reset, &exp);
  }
  if (exp) {
    (void)nexus_config_export(root);
  }
  if (reset) {
    printf("config: [ACTIONS] reset = 1: rebooting\n");
    sync_dir_of(path);
    sync();
    rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(200));
    rtems_fatal(RTEMS_FATAL_SOURCE_EXIT, 0); /* fatal extensions reset the USB PHY, BSP resets */
  }
}

static rtems_status_code media_listener(rtems_media_event event, rtems_media_state state,
                                        const char *src, const char *dest, void *arg)
{
  (void)arg;
  if (event == RTEMS_MEDIA_EVENT_MOUNT && state == RTEMS_MEDIA_STATE_SUCCESS && dest != NULL) {
    printf("config: USB drive %s mounted at %s\n", src ? src : "?", dest);
    pthread_mutex_lock(&ready_mtx);
    while (!config_ready) {
      pthread_cond_wait(&ready_cv, &ready_mtx);
    }
    pthread_mutex_unlock(&ready_mtx);
    char *d = strdup(dest);
    pthread_mutex_lock(&mtx);
    free(last_drive);
    last_drive = d;
    pthread_mutex_unlock(&mtx);
    process_drive(dest);
  } else if (event == RTEMS_MEDIA_EVENT_UNMOUNT) {
    pthread_mutex_lock(&mtx);
    if (last_drive != NULL && ((src && strcmp(src, last_drive) == 0) ||
                               (dest && strcmp(dest, last_drive) == 0))) {
      free(last_drive);
      last_drive = NULL;
    }
    pthread_mutex_unlock(&mtx);
  }
  return RTEMS_SUCCESSFUL;
}

static void media_start(void)
{
  static int started;
  if (started) {
    return;
  }
  started = 1;
  (void)rtems_filesystem_register(RTEMS_FILESYSTEM_TYPE_DOSFS, rtems_dosfs_initialize);
  rtems_status_code sc = rtems_bdbuf_init();
  if (sc == RTEMS_SUCCESSFUL) {
    sc = rtems_media_initialize();
  }
  if (sc == RTEMS_SUCCESSFUL) {
    sc = rtems_media_listener_add(media_listener, NULL);
  }
  if (sc == RTEMS_SUCCESSFUL) {
    sc = rtems_media_server_initialize(MEDIA_PRIO, MEDIA_STACK, RTEMS_DEFAULT_MODES,
                                       RTEMS_DEFAULT_ATTRIBUTES);
  }
  if (sc != RTEMS_SUCCESSFUL) {
    printf("config: media server not started (%s): no USB drive drop-in\n",
           rtems_status_text(sc));
  }
}

/* ---- init ---- */

static void seed_wifi_default(void)
{
  pthread_mutex_lock(&mtx);
  int have = cfg_has_wifi(&store);
  pthread_mutex_unlock(&mtx);
  if (have || NEXUS_WIFI_DEFAULT_SSID_LEN == 0) {
    return;
  }
  char ssid[33], psk[65];
  size_t sl = NEXUS_WIFI_DEFAULT_SSID_LEN, pl = NEXUS_WIFI_DEFAULT_PSK_LEN;
  if (sl >= sizeof(ssid) || pl >= sizeof(psk)) {
    return;
  }
  for (size_t i = 0; i < sl; ++i) {
    ssid[i] = (char)(nexus_wifi_default_ssid[i] ^ NEXUS_WIFI_DEFAULT_XOR);
  }
  ssid[sl] = '\0';
  for (size_t i = 0; i < pl; ++i) {
    psk[i] = (char)(nexus_wifi_default_psk[i] ^ NEXUS_WIFI_DEFAULT_XOR);
  }
  psk[pl] = '\0';
  int psk_ok = pl == 0 || (pl >= 8 && pl <= 64);
  if (sl < 1 || sl > 32 || !psk_ok) {
    printf("config: build-time Wi-Fi default invalid, ignored\n");
    memset(psk, 0, sizeof(psk));
    return;
  }
  cfg_update_t u;
  cfg_update_init(&u);
  if (kv_set(&u.set, "ssid_wlan", ssid) >= 0 && (pl == 0 || kv_set(&u.set, "pwd_wlan", psk) >= 0)) {
    (void)apply_update(&u, NULL);
    printf("config: Wi-Fi network \"%s\" seeded from the build defaults\n", ssid);
  }
  memset(psk, 0, sizeof(psk));
  cfg_update_free(&u);
}

void nexus_config_early(void)
{
  media_start();
}

static double mono_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int nexus_config_init(void)
{
  static int done;
  if (done) {
    return data_ok ? 0 : -1;
  }
  done = 1;
  kv_init(&store);
  double t0 = mono_s();
  flash_ok = qspi_nor_init() == 0;
  data_ok = flash_ok && data_mount() == 0;
  printf("config: flash and /data ready in %.0f ms\n", (mono_s() - t0) * 1e3);
  if (data_ok) {
    char *t = NULL;
    size_t len = 0;
    unlink(CONF_TMP);
    recover_swap(NEXUS_CONFIG_FILE);
    recover_swap(AUTH_KEYS);
    recover_swap(STATION_TOML);
    recover_swap(STATION_TOML ".bak");
    if (read_file(NEXUS_CONFIG_FILE, &t, &len, CFG_MAX_TEXT) == 0) {
      int n = cfg_load_flat(t, len, &store);
      free(t);
      printf("config: %d keys from %s\n", n < 0 ? 0 : n, NEXUS_CONFIG_FILE);
    } else {
      printf("config: no %s yet\n", NEXUS_CONFIG_FILE);
    }
  } else {
    printf("config: no persistent storage, configuration in RAM only\n");
  }
  seed_wifi_default();
  media_start();
  pthread_mutex_lock(&ready_mtx);
  config_ready = 1;
  pthread_cond_broadcast(&ready_cv);
  pthread_mutex_unlock(&ready_mtx);
  return data_ok ? 0 : -1;
}

/* ---- shell: config ... ---- */

static void show(void)
{
  pthread_mutex_lock(&mtx);
  for (size_t i = 0; i < store.n; ++i) {
    printf("%s = %s\n", store.k[i], cfg_display(store.k[i], store.v[i]));
  }
  printf("(%lu keys, %s)\n", (unsigned long)store.n,
         data_ok ? "stored in " NEXUS_CONFIG_FILE : "RAM only");
  pthread_mutex_unlock(&mtx);
  if (data_ok) {
    data_report("mounted");
  }
}

static void wifi_list(void)
{
  for (int s = 0; s < CFG_WIFI_SLOTS; ++s) {
    const char *ssid, *psk;
    if (nexus_config_wifi(s, &ssid, &psk) == 0) {
      printf("%d  %-32s  %s\n", s, ssid, psk[0] ? "psk " CFG_MASK : "open");
    }
  }
}

static int wifi_add(const char *ssid, const char *psk)
{
  int slot = -1;
  for (int s = 0; s < CFG_WIFI_SLOTS && slot < 0; ++s) {
    const char *cur;
    if (nexus_config_wifi(s, &cur, NULL) == 0 && strcmp(cur, ssid) == 0) {
      slot = s;
    }
  }
  for (int s = 0; s < CFG_WIFI_SLOTS && slot < 0; ++s) {
    if (nexus_config_wifi(s, NULL, NULL) != 0) {
      slot = s;
    }
  }
  if (slot < 0) {
    printf("config: all %d Wi-Fi slots are used (config wifi del <slot>)\n", CFG_WIFI_SLOTS);
    return 1;
  }
  char sk[24], pk[24];
  cfg_wifi_keys(slot, sk, pk, sizeof(sk));
  if (nexus_config_set(pk, psk) != 0 || nexus_config_set(sk, ssid) != 0) {
    return 1;
  }
  printf("config: Wi-Fi \"%s\" in slot %d\n", ssid, slot);
  return 0;
}

static int wifi_del(const char *arg)
{
  int slot = -1;
  if (isdigit((unsigned char)arg[0]) && arg[1] == '\0') {
    slot = arg[0] - '0';
  }
  for (int s = 0; s < CFG_WIFI_SLOTS && slot < 0; ++s) {
    const char *cur;
    if (nexus_config_wifi(s, &cur, NULL) == 0 && strcmp(cur, arg) == 0) {
      slot = s;
    }
  }
  if (slot < 0 || slot >= CFG_WIFI_SLOTS) {
    printf("config: no such Wi-Fi network\n");
    return 1;
  }
  char sk[24], pk[24];
  cfg_wifi_keys(slot, sk, pk, sizeof(sk));
  return nexus_config_set(sk, NULL) == 0 ? 0 : 1;
}

static int format_data(void)
{
  if (!flash_ok) {
    return 1;
  }
  if (data_ok && unmount(NEXUS_DATA_DIR) != 0) {
    printf("config: /data busy (errno %d)\n", errno);
    return 1;
  }
  pthread_mutex_lock(&mtx);
  data_ok = 0;
  int rc = erase_data_region() == 0 && mount_data_fs() == 0 ? 0 : -1;
  if (rc == 0) {
    data_ok = 1;
    (void)write_atomic(MARKER, "nexus-rtos\n", 11, 0644);
    rc = save_locked();
    (void)save_auth_keys_locked();
  }
  pthread_mutex_unlock(&mtx);
  printf(rc == 0 ? "config: /data formatted, configuration rewritten\n"
                 : "config: format failed, configuration in RAM only\n");
  return rc == 0 ? 0 : 1;
}

static const char usage[] =
  "config [show]                    all keys (secrets masked)\n"
  "config get KEY | set KEY VALUE... | unset KEY\n"
  "config wifi list | add SSID PSK|- | del SLOT|SSID   (slot 0 = highest priority)\n"
  "config apply FILE                apply a config.txt (file kept)\n"
  "config export [DIR]              DIR/config-current.txt (default: the USB drive)\n"
  "config station rollback          nexus2.toml.bak -> nexus2.toml\n"
  "config format yes                erase /data and rewrite the configuration\n";

static int shell_config(int argc, char **argv)
{
  if (argc < 2 || strcmp(argv[1], "show") == 0) {
    show();
    return 0;
  }
  const char *c = argv[1];
  if (strcmp(c, "get") == 0 && argc == 3) {
    const char *v = nexus_config_get(argv[2]);
    printf("%s\n", v ? cfg_display(argv[2], v) : "(unset)");
    return v ? 0 : 1;
  }
  if (strcmp(c, "set") == 0 && argc >= 4) {
    char val[256];
    size_t o = 0;
    for (int i = 3; i < argc; ++i) {
      o += (size_t)snprintf(val + o, o < sizeof(val) ? sizeof(val) - o : 0, "%s%s",
                            i > 3 ? " " : "", argv[i]);
    }
    if (o >= sizeof(val)) {
      printf("config: value too long\n");
      return 1;
    }
    return nexus_config_set(argv[2], val) == 0 ? 0 : 1;
  }
  if (strcmp(c, "unset") == 0 && argc == 3) {
    return nexus_config_set(argv[2], NULL) == 0 ? 0 : 1;
  }
  if (strcmp(c, "wifi") == 0 && argc >= 3) {
    if (strcmp(argv[2], "list") == 0) {
      wifi_list();
      return 0;
    }
    if (strcmp(argv[2], "add") == 0 && argc == 5) {
      return wifi_add(argv[3], strcmp(argv[4], "-") == 0 ? "" : argv[4]);
    }
    if (strcmp(argv[2], "del") == 0 && argc == 4) {
      return wifi_del(argv[3]);
    }
  }
  if (strcmp(c, "apply") == 0 && argc == 3) {
    return apply_config_file(argv[2], 0, NULL, NULL) == 0 ? 0 : 1;
  }
  if (strcmp(c, "export") == 0 && argc <= 3) {
    return nexus_config_export(argc == 3 ? argv[2] : NULL) == 0 ? 0 : 1;
  }
  if (strcmp(c, "station") == 0 && argc == 3 && strcmp(argv[2], "rollback") == 0) {
    return nexus_config_station_rollback() == 0 ? 0 : 1;
  }
  if (strcmp(c, "format") == 0 && argc == 3 && strcmp(argv[2], "yes") == 0) {
    return format_data();
  }
  fputs(usage, stdout);
  return 1;
}

void nexus_config_shell_register(void)
{
  (void)rtems_shell_add_cmd("config", "nexus", usage, shell_config);
}
