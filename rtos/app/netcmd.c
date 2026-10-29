/*
 * Nexus-BS RTOS network control (see netcmd.h).
 *
 * Static addresses: libbsd's dhcpcd (6.2.1, MASTER_ONLY: no control socket, no signals) reads
 * /etc/dhcpcd.conf again for an interface when the interface arrives (RTM_IFANNOUNCE) and when
 * its carrier comes back (handle_carrier -> configure_interface -> read_config). A per-interface
 * block with `static ip_address/routers/domain_name_servers` makes dhcpcd bind that address
 * itself (dhcp_static, reason STATIC: address, routes and the DNS hook as for a lease), so
 * static and DHCP interfaces share one route/DNS owner and hotplug re-applies both. A runtime
 * change rewrites the file (temp + rename) and bounces IFF_UP on the interface: the down
 * RTM_IFINFO makes dhcpcd drop it, the up one makes it re-read the file and start again.
 *
 * Release/renew: dhcpcd cannot be signalled on RTEMS. `net release` deletes the interface's
 * IPv4 addresses (RTM_DELADDR -> dhcp_drop "EXPIRE": routes and DNS removed) and the lease file;
 * no DHCPRELEASE is sent (the server keeps the lease until it expires). `net renew` bounces the
 * interface (INIT-REBOOT with the lease file, or DISCOVER after a release).
 *
 * Every libbsd program call resets newlib's getopt (optind = 0) under one lock.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <machine/rtems-bsd-commands.h>
#include <rtems.h>
#include <rtems/bsd/bsd.h>
#include <rtems/bsd/modules.h>
#include <rtems/libio.h>
#include <rtems/shell.h>

#include "aicwf_wifi.h"
#include "config_store.h"
#include "netcmd.h"

/* tun/tap: libbsd's if_tuntap.c needs devfs cloning (dev_ref, clone_create, make_dev_alias_p)
 * that RTEMS libbsd does not provide, so the module is not linked; `net tun` reports the
 * cloner as absent and the TETRA IP data carrier needs another transport on RTEMS */

#define DHCPCD_CONF "/etc/dhcpcd.conf"
#define DHCPCD_LEASE "/var/db/dhcpcd-%s.lease"
#define WPA_CONF "/etc/wpa_supplicant.conf"
#define WPA_DIR "/var/run/wpa_supplicant"

/* ---- small helpers ---- */

static pthread_mutex_t getopt_lock = PTHREAD_MUTEX_INITIALIZER;

/* a libbsd program (ifconfig, arp, netstat, ping, ...): newlib getopt state reset first */
static int bsd_prog(int (*fn)(int, char **), int argc, char **argv)
{
  pthread_mutex_lock(&getopt_lock);
  optind = 0;
  int rc = fn(argc, argv);
  pthread_mutex_unlock(&getopt_lock);
  return rc;
}

static double mono_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void spawn_thread(void *(*fn)(void *), void *arg, int prio, size_t stack)
{
  pthread_attr_t at;
  struct sched_param sp = {.sched_priority = prio};
  pthread_t t;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, stack);
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&at, SCHED_FIFO);
  pthread_attr_setschedparam(&at, &sp);
  if (pthread_create(&t, &at, fn, arg) == 0) {
    pthread_detach(t);
  } else {
    printf("net: thread start failed\n");
  }
  pthread_attr_destroy(&at);
}

static int write_file_atomic(const char *path, const char *data, size_t len, mode_t mode)
{
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, mode);
  if (fd < 0) {
    return -1;
  }
  ssize_t n = write(fd, data, len);
  close(fd);
  if (n != (ssize_t)len || rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

static int parse_ip4(const char *s, struct in_addr *a)
{
  return s != NULL && inet_pton(AF_INET, s, a) == 1 ? 0 : -1;
}

/* "a.b.c.d[/p]" -> address text and prefix (default_prefix when absent) */
static int parse_ip_prefix(const char *s, char *ip, size_t cap, int *prefix, int default_prefix)
{
  struct in_addr a;
  const char *slash = s != NULL ? strchr(s, '/') : NULL;
  size_t n = slash != NULL ? (size_t)(slash - s) : (s != NULL ? strlen(s) : 0);
  if (n == 0 || n >= cap) {
    return -1;
  }
  memcpy(ip, s, n);
  ip[n] = '\0';
  if (parse_ip4(ip, &a) != 0) {
    return -1;
  }
  *prefix = default_prefix;
  if (slash != NULL) {
    char *e;
    long p = strtol(slash + 1, &e, 10);
    if (*e != '\0' || p < 1 || p > 32) {
      return -1;
    }
    *prefix = (int)p;
  }
  return 0;
}

static int mask_to_prefix(uint32_t mask_net)
{
  uint32_t m = ntohl(mask_net);
  int p = 0;
  while (p < 32 && (m & (0x80000000u >> p)) != 0) {
    ++p;
  }
  return p;
}

/* ---- interface ioctls ---- */

static int if_sock(void)
{
  return socket(AF_INET, SOCK_DGRAM, 0);
}

static int if_get_flags(const char *ifn, int *flags)
{
  struct ifreq ifr;
  int s = if_sock();
  if (s < 0) {
    return -1;
  }
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifn);
  int rc = ioctl(s, SIOCGIFFLAGS, &ifr);
  close(s);
  if (rc == 0) {
    *flags = (ifr.ifr_flags & 0xffff) | (ifr.ifr_flagshigh << 16);
  }
  return rc;
}

static int if_set_up(const char *ifn, bool up)
{
  struct ifreq ifr;
  int s = if_sock();
  if (s < 0) {
    return -1;
  }
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifn);
  int rc = ioctl(s, SIOCGIFFLAGS, &ifr);
  if (rc == 0) {
    int f = (ifr.ifr_flags & 0xffff) | (ifr.ifr_flagshigh << 16);
    f = up ? (f | IFF_UP) : (f & ~IFF_UP);
    ifr.ifr_flags = (short)(f & 0xffff);
    ifr.ifr_flagshigh = (short)(f >> 16);
    rc = ioctl(s, SIOCSIFFLAGS, &ifr);
  }
  int e = errno;
  close(s);
  errno = e;
  return rc;
}

/* down + up: dhcpcd drops the interface on the first RTM_IFINFO and re-reads dhcpcd.conf and
 * starts again on the second (Wi-Fi: net80211 re-associates through wpa_supplicant) */
static int if_bounce(const char *ifn)
{
  if (if_nametoindex(ifn) == 0) {
    errno = ENXIO;
    return -1;
  }
  if (if_set_up(ifn, false) != 0) {
    return -1;
  }
  return if_set_up(ifn, true);
}

/* delete every IPv4 address (bounded) */
static int if_del_inet(const char *ifn)
{
  int s = if_sock(), n = 0;
  if (s < 0) {
    return -1;
  }
  for (int i = 0; i < 16; ++i) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifn);
    if (ioctl(s, SIOCGIFADDR, &ifr) != 0) {
      break; /* EADDRNOTAVAIL: none left */
    }
    if (ioctl(s, SIOCDIFADDR, &ifr) != 0) {
      break;
    }
    ++n;
  }
  close(s);
  return n;
}

static int if_add_inet(const char *ifn, struct in_addr addr, int prefix, const struct in_addr *dst)
{
  struct ifaliasreq ifra;
  int s = if_sock();
  if (s < 0) {
    return -1;
  }
  memset(&ifra, 0, sizeof(ifra));
  snprintf(ifra.ifra_name, sizeof(ifra.ifra_name), "%s", ifn);
  struct sockaddr_in *a = (struct sockaddr_in *)&ifra.ifra_addr;
  struct sockaddr_in *b = (struct sockaddr_in *)&ifra.ifra_broadaddr; /* dstaddr on p2p */
  struct sockaddr_in *m = (struct sockaddr_in *)&ifra.ifra_mask;
  a->sin_len = b->sin_len = m->sin_len = sizeof(struct sockaddr_in);
  a->sin_family = b->sin_family = m->sin_family = AF_INET;
  a->sin_addr = addr;
  m->sin_addr.s_addr = prefix == 0 ? 0 : htonl(0xffffffffu << (32 - prefix));
  if (dst != NULL) {
    b->sin_addr = *dst;
  } else {
    b->sin_addr.s_addr = addr.s_addr | ~m->sin_addr.s_addr;
  }
  int rc = ioctl(s, SIOCAIFADDR, &ifra);
  int e = errno;
  close(s);
  errno = e;
  return rc;
}

/* ---- per-interface address configuration ---- */

enum { NX_MODE_CFG = 0, NX_MODE_DHCP = 1, NX_MODE_STATIC = 2 };

typedef struct {
  char ifn[IFNAMSIZ];
  int mode;
  char ip[16];
  int prefix;
  char gw[16];
  char dns[64];
} nx_ifcfg_t;

#define NX_OVR 6
static nx_ifcfg_t ovr[NX_OVR]; /* runtime overrides (net dhcp/static without --save) */
static pthread_mutex_t cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static bool cfg_loaded;          /* nx_net_start ran: the configuration is in RAM */

/* the documented keys: [WLAN] ipaddr_wlan, gateway_wlan for every wlanN; [USB_ETHERNET]
 * ipaddr_eth, netmask_eth, gateway_eth for every ueN; [NETWORK] dns for both */
static int cfg_family(const char *ifn)
{
  if (strncmp(ifn, "wlan", 4) == 0) {
    return 1;
  }
  if (strncmp(ifn, "ue", 2) == 0 && isdigit((unsigned char)ifn[2])) {
    return 2;
  }
  return 0;
}

static const char *cfg_get(const char *key)
{
  return cfg_loaded ? nexus_config_get(key) : NULL;
}

/* effective settings of ifn: NX_MODE_STATIC with c filled, or NX_MODE_DHCP; caller holds lock */
static int eff_cfg_locked(const char *ifn, nx_ifcfg_t *c)
{
  memset(c, 0, sizeof(*c));
  snprintf(c->ifn, sizeof(c->ifn), "%s", ifn);
  for (int i = 0; i < NX_OVR; ++i) {
    if (ovr[i].ifn[0] != '\0' && strcmp(ovr[i].ifn, ifn) == 0 && ovr[i].mode != NX_MODE_CFG) {
      *c = ovr[i];
      return c->mode;
    }
  }
  const char *ip = NULL, *gw = NULL, *mask = NULL;
  switch (cfg_family(ifn)) {
  case 1:
    ip = cfg_get("ipaddr_wlan");
    gw = cfg_get("gateway_wlan");
    break;
  case 2:
    ip = cfg_get("ipaddr_eth");
    gw = cfg_get("gateway_eth");
    mask = cfg_get("netmask_eth");
    break;
  default:
    break;
  }
  int dflt = 24;
  struct in_addr m;
  if (mask != NULL && parse_ip4(mask, &m) == 0) {
    dflt = mask_to_prefix(m.s_addr);
  }
  if (ip == NULL || ip[0] == '\0' || parse_ip_prefix(ip, c->ip, sizeof(c->ip), &c->prefix, dflt) != 0) {
    c->mode = NX_MODE_DHCP;
    return c->mode;
  }
  c->mode = NX_MODE_STATIC;
  struct in_addr g;
  if (gw != NULL && parse_ip4(gw, &g) == 0) {
    snprintf(c->gw, sizeof(c->gw), "%s", gw);
  }
  const char *dns = cfg_get("dns");
  snprintf(c->dns, sizeof(c->dns), "%s", dns != NULL ? dns : "");
  return c->mode;
}

/* the dhcpcd.conf block of one interface */
static size_t conf_block(const char *ifn, char *out, size_t cap)
{
  nx_ifcfg_t c;
  size_t o = 0;
  pthread_mutex_lock(&cfg_lock);
  int mode = eff_cfg_locked(ifn, &c);
  pthread_mutex_unlock(&cfg_lock);
  o += (size_t)snprintf(out + o, cap - o, "interface %s\n", ifn);
  /* one client identifier per interface (two adapters on one LAN must not share a lease);
   * wlan0 keeps the historical id so existing reservations stay valid */
  if (strcmp(ifn, "wlan0") != 0 && o < cap) {
    o += (size_t)snprintf(out + o, cap - o, "clientid nexus-rtos-%s\n", ifn);
  }
  if (mode == NX_MODE_STATIC && o < cap) {
    o += (size_t)snprintf(out + o, cap - o, "static ip_address=%s/%d\n", c.ip, c.prefix);
    if (c.gw[0] != '\0' && o < cap) {
      o += (size_t)snprintf(out + o, cap - o, "static routers=%s\n", c.gw);
    }
    /* no DNS configured: the gateway (home routers answer DNS) */
    const char *dns = c.dns[0] != '\0' ? c.dns : c.gw;
    if (dns[0] != '\0' && o < cap) {
      o += (size_t)snprintf(out + o, cap - o, "static domain_name_servers=%s\n", dns);
    }
  }
  return o < cap ? o : cap - 1;
}

/* interfaces with a block: the adapters' names plus overridden ones */
static const char *const known_ifs[] = {"wlan0", "wlan1", "wlan2", "ue0", "ue1", NULL};
#define NX_BLOCKS (5 + NX_OVR)
static struct {
  char ifn[IFNAMSIZ];
  char block[256];
} applied[NX_BLOCKS];
static pthread_mutex_t conf_lock = PTHREAD_MUTEX_INITIALIZER;

static int if_list(char names[][IFNAMSIZ], int max)
{
  int n = 0;
  for (int i = 0; known_ifs[i] != NULL && n < max; ++i) {
    snprintf(names[n++], IFNAMSIZ, "%s", known_ifs[i]);
  }
  pthread_mutex_lock(&cfg_lock);
  for (int i = 0; i < NX_OVR && n < max; ++i) {
    bool dup = ovr[i].ifn[0] == '\0';
    for (int j = 0; j < n && !dup; ++j) {
      dup = strcmp(names[j], ovr[i].ifn) == 0;
    }
    if (!dup) {
      snprintf(names[n++], IFNAMSIZ, "%s", ovr[i].ifn);
    }
  }
  pthread_mutex_unlock(&cfg_lock);
  return n;
}

/*
 * Write dhcpcd.conf; with bounce, re-start every present interface whose block changed since
 * the last write. Global options: fast lease (no ARP probe: the kernel still announces every
 * new address with a gratuitous ARP in arp_ifinit), IPv4 only, DNS/NTP to the hook in main.c,
 * the host name to the server. tun/tap are left to their owner (noipv4/noipv6 instead of
 * denyinterfaces: dhcpcd 6.2.1 appends a denyinterfaces list to a global on every re-read).
 */
static void conf_apply(bool bounce)
{
  static char buf[4096];
  char names[NX_BLOCKS][IFNAMSIZ];
  char changed[NX_BLOCKS][IFNAMSIZ];
  int nchanged = 0;
  pthread_mutex_lock(&conf_lock);
  size_t o = (size_t)snprintf(buf, sizeof(buf),
                              "clientid nexus-rtos\nbackground\nhostname\n"
                              "option domain_name_servers, domain_name, ntp_servers\n"
                              "noarp\nipv4only\nnoipv6rs\n");
  for (int t = 0; t < 4 && o < sizeof(buf) - 64; ++t) {
    o += (size_t)snprintf(buf + o, sizeof(buf) - o, "interface tun%d\nnoipv4\nnoipv6\n", t);
  }
  for (int t = 0; t < 2 && o < sizeof(buf) - 64; ++t) {
    o += (size_t)snprintf(buf + o, sizeof(buf) - o, "interface tap%d\nnoipv4\nnoipv6\n", t);
  }
  int n = if_list(names, NX_BLOCKS);
  for (int i = 0; i < n; ++i) {
    char blk[256];
    conf_block(names[i], blk, sizeof(blk));
    if (o + strlen(blk) < sizeof(buf)) {
      memcpy(buf + o, blk, strlen(blk));
      o += strlen(blk);
    }
    int slot = -1;
    for (int j = 0; j < NX_BLOCKS && slot < 0; ++j) {
      if (strcmp(applied[j].ifn, names[i]) == 0) {
        slot = j;
      }
    }
    for (int j = 0; j < NX_BLOCKS && slot < 0; ++j) {
      if (applied[j].ifn[0] == '\0') {
        slot = j;
        snprintf(applied[j].ifn, sizeof(applied[j].ifn), "%s", names[i]);
      }
    }
    if (slot >= 0 && strcmp(applied[slot].block, blk) != 0) {
      snprintf(applied[slot].block, sizeof(applied[slot].block), "%s", blk);
      snprintf(changed[nchanged++], IFNAMSIZ, "%s", names[i]);
    }
  }
  if (write_file_atomic(DHCPCD_CONF, buf, o, 0644) != 0) {
    printf("net: %s not written\n", DHCPCD_CONF);
  }
  pthread_mutex_unlock(&conf_lock);
  for (int i = 0; bounce && i < nchanged; ++i) {
    if (if_nametoindex(changed[i]) != 0) {
      printf("net: %s address settings changed, re-applied\n", changed[i]);
      (void)if_bounce(changed[i]);
    }
  }
}

void nx_net_dhcpcd_conf_write(void)
{
  conf_apply(false);
}

/* ---- dhcpcd task ---- */

static volatile int dhcpcd_state; /* 0 not started, 1 running, 2 ended */
static volatile int dhcpcd_exit;

static void dhcpcd_destroy(const rtems_dhcpcd_config *config, int exit_code)
{
  (void)config;
  dhcpcd_exit = exit_code;
  dhcpcd_state = 2;
  /* rtems_dhcpcd_start() refuses a second start (dhcpcd_initialized stays set): leases are
   * not renewed and hotplugged interfaces are not configured until reboot */
  printf("net: dhcpcd ended (exit %d): no DHCP until reboot\n", exit_code);
}

static void dhcpcd_prepare(const rtems_dhcpcd_config *config, int argc, char **argv)
{
  (void)config;
  (void)argc;
  (void)argv;
  dhcpcd_state = 1;
}

static char dhcpcd_a0[] = "dhcpcd";
static char *dhcpcd_argv[] = {dhcpcd_a0, NULL};
static const rtems_dhcpcd_config dhcpcd_cfg = {
  .priority = 0, /* libbsd default: RTEMS_MAXIMUM_PRIORITY - 1 */
  .argc = 1,
  .argv = dhcpcd_argv,
  .prepare = dhcpcd_prepare,
  .destroy = dhcpcd_destroy,
};

const rtems_dhcpcd_config *nx_net_dhcpcd_config(void)
{
  return &dhcpcd_cfg;
}

/* ---- wpa_supplicant control socket client (ctrl_iface_unix.c, PF_UNIX datagrams) ---- */

static unsigned wpa_seq;

typedef struct {
  int fd;
  char local[48];
} wpa_conn_t;

static int wpa_open(wpa_conn_t *c, const char *ifn)
{
  struct sockaddr_un l, r;
  c->fd = socket(PF_UNIX, SOCK_DGRAM, 0);
  if (c->fd < 0) {
    return -1;
  }
  int rcv = 32 * 1024; /* SCAN_RESULTS can be several KiB */
  setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
  memset(&l, 0, sizeof(l));
  memset(&r, 0, sizeof(r));
  l.sun_family = r.sun_family = AF_UNIX;
  snprintf(c->local, sizeof(c->local), "/var/run/nxwpa-%u", __atomic_add_fetch(&wpa_seq, 1, __ATOMIC_RELAXED));
  snprintf(l.sun_path, sizeof(l.sun_path), "%s", c->local);
  snprintf(r.sun_path, sizeof(r.sun_path), "%s/%s", WPA_DIR, ifn);
  l.sun_len = (unsigned char)SUN_LEN(&l);
  r.sun_len = (unsigned char)SUN_LEN(&r);
  (void)unlink(c->local);
  if (bind(c->fd, (struct sockaddr *)&l, l.sun_len) != 0 ||
      connect(c->fd, (struct sockaddr *)&r, r.sun_len) != 0) {
    close(c->fd);
    (void)unlink(c->local);
    c->fd = -1;
    return -1;
  }
  return 0;
}

static void wpa_close(wpa_conn_t *c)
{
  if (c->fd >= 0) {
    close(c->fd);
    (void)unlink(c->local);
    c->fd = -1;
  }
}

/* next datagram within timeout_ms; events ('<' prefix) skipped unless want_events */
static int wpa_recv(wpa_conn_t *c, char *buf, size_t cap, int timeout_ms, bool want_events)
{
  double end = mono_s() + timeout_ms / 1000.0;
  for (;;) {
    int left = (int)((end - mono_s()) * 1000.0);
    if (left <= 0) {
      return -1;
    }
    struct pollfd p = {.fd = c->fd, .events = POLLIN};
    if (poll(&p, 1, left) <= 0) {
      return -1;
    }
    ssize_t n = recv(c->fd, buf, cap - 1, 0);
    if (n < 0) {
      return -1;
    }
    buf[n] = '\0';
    if (buf[0] != '<' || want_events) {
      return (int)n;
    }
  }
}

static int wpa_cmd_on(wpa_conn_t *c, const char *cmd, char *reply, size_t cap, int timeout_ms)
{
  if (send(c->fd, cmd, strlen(cmd), 0) < 0) {
    return -1;
  }
  return wpa_recv(c, reply, cap, timeout_ms, false);
}

static int wpa_request(const char *ifn, const char *cmd, char *reply, size_t cap, int timeout_ms)
{
  wpa_conn_t c;
  if (wpa_open(&c, ifn) != 0) {
    return -1;
  }
  int n = wpa_cmd_on(&c, cmd, reply, cap, timeout_ms);
  wpa_close(&c);
  return n;
}

/* value of "key=" in a STATUS / SIGNAL_POLL reply */
static bool kv_line(const char *text, const char *key, char *out, size_t cap)
{
  size_t k = strlen(key);
  for (const char *p = text; p != NULL && *p != '\0';) {
    if (strncmp(p, key, k) == 0 && p[k] == '=') {
      const char *v = p + k + 1;
      size_t n = strcspn(v, "\n");
      if (n >= cap) {
        n = cap - 1;
      }
      memcpy(out, v, n);
      out[n] = '\0';
      return true;
    }
    p = strchr(p, '\n');
    if (p != NULL) {
      ++p;
    }
  }
  return false;
}

/* ---- net80211 Wi-Fi supervisor ---- */

/*
 * libbsd runs wpa_supplicant's main() under one global mutex (rtems-kernel-wpa-supplicant.c):
 * a second instance blocks until the first one returns. So one instance serves every net80211
 * interface (-i a ... -N -i b ...); it is restarted when the set of interfaces changes. With
 * CONFIG_TERMINATE_ONLASTIF it ends by itself when its last interface departs (stick
 * unplugged); the next stick gets a fresh clone and a fresh instance. Clones are wlan1/wlan2:
 * the AIC driver attaches its station as wlan0 unconditionally (if_aicwf_usb.c), a clone
 * holding wlan0 would duplicate the name when an AIC stick is plugged in later.
 */
#define W_MAX 2
static const char *w_def_ssid, *w_def_psk;
static pthread_mutex_t w_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t w_cond = PTHREAD_COND_INITIALIZER;
static bool w_running;                   /* the supplicant task is in main() */
static char w_run_set[W_MAX][IFNAMSIZ];  /* interfaces it was started with */
static int w_run_n;
static volatile bool w_reconf;           /* configuration changed: rewrite + RECONFIGURE */
static char w_rt_ssid[33], w_rt_psk[65]; /* `wifi connect` network (runtime, highest priority) */
static bool w_rt_set;
static volatile bool w_parent_present;

typedef struct {
  int argc;
  char *argv[12];
  char store[12][40];
} w_args_t;

#ifdef RTEMS_BSD_MODULE_USR_SBIN_WPA_SUPPLICANT
static void *w_supplicant_task(void *arg)
{
  w_args_t *a = arg;
  int rc = rtems_bsd_command_wpa_supplicant(a->argc, a->argv);
  free(a);
  pthread_mutex_lock(&w_lock);
  w_running = false;
  w_run_n = 0;
  pthread_cond_broadcast(&w_cond);
  pthread_mutex_unlock(&w_lock);
  printf("wifi: wpa_supplicant ended (%d)\n", rc);
  return NULL;
}
#endif

/* append to a fixed buffer (the file holds up to 6 networks of < 200 bytes) */
typedef struct {
  char d[2048];
  size_t n;
  bool overflow;
} w_buf_t;

static void w_put(w_buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void w_put(w_buf_t *b, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(b->d + b->n, sizeof(b->d) - b->n, fmt, ap);
  va_end(ap);
  if (k < 0 || (size_t)k >= sizeof(b->d) - b->n) {
    b->overflow = true;
    b->n = sizeof(b->d) - 1;
  } else {
    b->n += (size_t)k;
  }
}

static void w_quote(w_buf_t *b, const char *k, const char *v)
{
  w_put(b, "\t%s=\"", k);
  for (; *v != '\0'; v++) {
    w_put(b, "%s%c", (*v == '"' || *v == '\\') ? "\\" : "", *v);
  }
  w_put(b, "\"\n");
}

static void w_network(w_buf_t *b, const char *ssid, const char *psk, int prio)
{
  w_put(b, "network={\n");
  w_quote(b, "ssid", ssid);
  size_t n = psk != NULL ? strlen(psk) : 0;
  if (n == 0) {
    w_put(b, "\tkey_mgmt=NONE\n");
  } else if (n == 64 && strspn(psk, "0123456789abcdefABCDEF") == 64) {
    w_put(b, "\tpsk=%s\n", psk); /* raw 256-bit key, unquoted */
  } else {
    w_quote(b, "psk", psk);
  }
  w_put(b, "\tscan_ssid=1\n\tpriority=%d\n}\n", prio);
}

/* every configured network (config.txt [WLAN] slots, the first preferred), the runtime
 * `wifi connect` network above them; control socket for the `wifi` command. Returns the
 * network count (-1 not written); *changed = the content differs from the last write (only
 * then RECONFIGURE: it de-authenticates the current association) */
static uint32_t w_conf_hash;

static int w_conf(bool *changed)
{
  w_buf_t *b = calloc(1, sizeof(*b));
  if (b == NULL) {
    return -1;
  }
  w_put(b, "ctrl_interface=%s\nap_scan=1\n", WPA_DIR);
  int count = 0;
  pthread_mutex_lock(&w_lock);
  if (w_rt_set) {
    w_network(b, w_rt_ssid, w_rt_psk, 32);
    count++;
  }
  pthread_mutex_unlock(&w_lock);
  for (int i = 0; i < 16; i++) {
    const char *ssid = NULL, *psk = NULL;
    if (nexus_config_wifi(i, &ssid, &psk) == 0 && ssid != NULL && ssid[0] != '\0') {
      w_network(b, ssid, psk, 16 - i);
      count++;
    }
  }
  if (count == 0 && w_def_ssid != NULL && w_def_ssid[0] != '\0') {
    w_network(b, w_def_ssid, w_def_psk, 1);
    count = 1;
  }
  uint32_t h = 2166136261u; /* FNV-1a: change detection only */
  for (size_t i = 0; i < b->n; ++i) {
    h = (h ^ (uint8_t)b->d[i]) * 16777619u;
  }
  int rc = -1;
  if (!b->overflow && write_file_atomic(WPA_CONF, b->d, b->n, 0600) == 0) {
    rc = count;
    if (changed != NULL) {
      *changed = h != w_conf_hash;
    }
    w_conf_hash = h;
  }
  memset(b, 0, sizeof(*b)); /* passphrases */
  free(b);
  return rc;
}

/* the clone of parent dev, if any: net.wlan.<unit>.%parent */
static int w_clone_of(const char *dev, char *ifn, size_t cap)
{
  for (int u = 1; u <= W_MAX; ++u) {
    char name[24], parent[32];
    size_t len = sizeof(parent) - 1;
    snprintf(name, sizeof(name), "net.wlan.%d.%%parent", u);
    if (sysctlbyname(name, parent, &len, NULL, 0) == 0) {
      parent[len < sizeof(parent) ? len : sizeof(parent) - 1] = '\0';
      if (strcmp(parent, dev) == 0) {
        snprintf(ifn, cap, "wlan%d", u);
        return 0;
      }
    }
  }
  return -1;
}

/* net80211 parents except the AIC driver (which has its own station) */
static int w_parents(char devs[][24], int max)
{
  char list[160];
  size_t len = sizeof(list) - 1;
  int n = 0;
  if (sysctlbyname("net.wlan.devices", list, &len, NULL, 0) != 0 || len == 0) {
    return 0;
  }
  list[len < sizeof(list) ? len : sizeof(list) - 1] = '\0';
  char *save = NULL;
  for (char *t = strtok_r(list, " ", &save); t != NULL && n < max; t = strtok_r(NULL, " ", &save)) {
    if (strncmp(t, "aicwf", 5) != 0 && strlen(t) < 24) {
      snprintf(devs[n++], 24, "%s", t);
    }
  }
  return n;
}

static void w_start_supplicant(char set[][IFNAMSIZ], int n)
{
  int nets = w_conf(NULL);
  if (nets < 0) {
    printf("wifi: %s not written\n", WPA_CONF);
    return;
  }
  w_args_t *a = calloc(1, sizeof(*a));
  if (a == NULL) {
    return;
  }
  int k = 0;
#define W_ARG(s) (snprintf(a->store[k], sizeof(a->store[k]), "%s", (s)), a->argv[k] = a->store[k], k++)
  W_ARG("wpa_supplicant");
  W_ARG("-s");
  for (int i = 0; i < n; ++i) {
    char iarg[24];
    if (i > 0) {
      W_ARG("-N");
    }
    W_ARG("-Dbsd");
    snprintf(iarg, sizeof(iarg), "-i%s", set[i]);
    W_ARG(iarg);
    W_ARG("-c" WPA_CONF);
  }
#undef W_ARG
  a->argc = k;
  a->argv[k] = NULL;
  (void)rtems_mkdir(WPA_DIR, S_IRWXU);
  pthread_mutex_lock(&w_lock);
  w_running = true;
  w_run_n = n;
  for (int i = 0; i < n; ++i) {
    snprintf(w_run_set[i], IFNAMSIZ, "%s", set[i]);
  }
  pthread_mutex_unlock(&w_lock);
  printf("wifi: wpa_supplicant on %s%s%s, %d network(s)\n", set[0], n > 1 ? " " : "", n > 1 ? set[1] : "",
         nets);
#ifdef RTEMS_BSD_MODULE_USR_SBIN_WPA_SUPPLICANT
  /* libbsd's fork wrapper uses 8 x RTEMS_MINIMUM_STACK_SIZE at the lowest priority */
  spawn_thread(w_supplicant_task, a, 1, 64 * 1024);
#else
  free(a);
  pthread_mutex_lock(&w_lock);
  w_running = false;
  w_run_n = 0;
  pthread_mutex_unlock(&w_lock);
  printf("wifi: no wpa_supplicant in this libbsd build\n");
#endif
}

/* ask the running instance to end; wait up to 5 s */
static bool w_stop_supplicant(void)
{
  char ifs[W_MAX][IFNAMSIZ], r[32];
  int n;
  pthread_mutex_lock(&w_lock);
  n = w_run_n;
  memcpy(ifs, w_run_set, sizeof(ifs));
  bool running = w_running;
  pthread_mutex_unlock(&w_lock);
  if (!running) {
    return true;
  }
  for (int i = 0; i < n; ++i) {
    if (wpa_request(ifs[i], "TERMINATE", r, sizeof(r), 2000) > 0) {
      break;
    }
  }
  struct timespec dl;
  clock_gettime(CLOCK_REALTIME, &dl);
  dl.tv_sec += 5;
  pthread_mutex_lock(&w_lock);
  while (w_running) {
    if (pthread_cond_timedwait(&w_cond, &w_lock, &dl) == ETIMEDOUT) {
      break;
    }
  }
  running = w_running;
  pthread_mutex_unlock(&w_lock);
  if (running) {
    printf("wifi: wpa_supplicant did not end, retrying later\n");
  }
  return !running;
}

static void *w_main(void *arg)
{
  (void)arg;
  unsigned backoff_s = 0;
  double last_start = -1e9;
  for (;;) {
    char devs[W_MAX][24], set[W_MAX][IFNAMSIZ];
    int nd = w_parents(devs, W_MAX), ns = 0;
    w_parent_present = nd > 0;
    for (int i = 0; i < nd; ++i) {
      char ifn[IFNAMSIZ];
      if (w_clone_of(devs[i], ifn, sizeof(ifn)) != 0) {
        /* first free unit in wlan1..wlanW_MAX */
        ifn[0] = '\0';
        for (int u = 1; u <= W_MAX && ifn[0] == '\0'; ++u) {
          char t[IFNAMSIZ];
          snprintf(t, sizeof(t), "wlan%d", u);
          if (if_nametoindex(t) == 0) {
            snprintf(ifn, sizeof(ifn), "%s", t);
          }
        }
        if (ifn[0] == '\0') {
          continue;
        }
        printf("wifi: %s (net80211), creating %s\n", devs[i], ifn);
        char *av[] = {"ifconfig", ifn, "create", "wlandev", devs[i], "up", NULL};
        if (bsd_prog(rtems_bsd_command_ifconfig, 6, av) != 0 || if_nametoindex(ifn) == 0) {
          printf("wifi: %s on %s not created\n", ifn, devs[i]);
          continue;
        }
      }
      snprintf(set[ns++], IFNAMSIZ, "%s", ifn);
    }
    if (ns == 2 && strcmp(set[0], set[1]) > 0) {
      char t[IFNAMSIZ];
      memcpy(t, set[0], IFNAMSIZ);
      memcpy(set[0], set[1], IFNAMSIZ);
      memcpy(set[1], t, IFNAMSIZ);
    }
    pthread_mutex_lock(&w_lock);
    bool running = w_running;
    bool same = running && w_run_n == ns;
    for (int i = 0; same && i < ns; ++i) {
      same = strcmp(w_run_set[i], set[i]) == 0;
    }
    pthread_mutex_unlock(&w_lock);

    if (running && !same) {
      (void)w_stop_supplicant(); /* the set changed: restart with the new set below */
    } else if (running && w_reconf) {
      w_reconf = false;
      bool changed = false;
      if (w_conf(&changed) >= 0 && changed) {
        char r[32];
        for (int i = 0; i < ns; ++i) {
          (void)wpa_request(set[i], "RECONFIGURE", r, sizeof(r), 2000);
        }
      }
    }
    pthread_mutex_lock(&w_lock);
    running = w_running;
    pthread_mutex_unlock(&w_lock);
    if (!running && ns > 0 && mono_s() - last_start >= backoff_s) {
      /* an instance that ends within 10 s of its start backs off (1, 2, 4 .. 60 s) */
      if (mono_s() - last_start < 10.0) {
        backoff_s = backoff_s == 0 ? 1 : (backoff_s * 2 > 60 ? 60 : backoff_s * 2);
      } else {
        backoff_s = 0;
      }
      w_reconf = false;
      last_start = mono_s();
      w_start_supplicant(set, ns);
    }
    usleep(500000); /* parents arrive by USB hotplug; nothing else to wait on */
  }
  return NULL;
}

static int wifi_fallback_cmd(int argc, char **argv);

/* called after nexus_wifi_start(): `wifi` exists unless the AIC driver failed to start */
void nx_wifi80211_start(const char *ssid, const char *psk)
{
#ifdef RTEMS_BSD_MODULE_NET80211
  w_def_ssid = ssid;
  w_def_psk = psk;
  if (rtems_shell_lookup_cmd("wifi") == NULL) {
    rtems_shell_add_cmd("wifi", "network", "wifi status|scan|connect <ssid> [psk]|disconnect|reconnect",
                        wifi_fallback_cmd);
  }
  spawn_thread(w_main, NULL, 5, 16384);
#else
  (void)ssid;
  (void)psk;
#endif
}

int nx_wifi80211_present(void)
{
  return w_parent_present ? 1 : 0;
}

static int w_first_if(char *ifn, size_t cap)
{
  pthread_mutex_lock(&w_lock);
  int ok = w_running && w_run_n > 0;
  if (ok) {
    snprintf(ifn, cap, "%s", w_run_set[0]);
  }
  pthread_mutex_unlock(&w_lock);
  return ok ? 0 : -1;
}

static void w_status_line(const char *ifn)
{
  char st[1024], sig[256], v[80];
  if (wpa_request(ifn, "STATUS", st, sizeof(st), 2000) <= 0) {
    printf("wifi: %s: wpa_supplicant not answering\n", ifn);
    return;
  }
  printf("wifi: %s (net80211)", ifn);
  if (kv_line(st, "wpa_state", v, sizeof(v))) {
    printf(" %s", v);
  }
  if (kv_line(st, "ssid", v, sizeof(v))) {
    printf(" \"%s\"", v);
  }
  if (kv_line(st, "bssid", v, sizeof(v))) {
    printf(" bssid %s", v);
  }
  if (kv_line(st, "freq", v, sizeof(v))) {
    printf(" %s MHz", v);
  }
  if (kv_line(st, "key_mgmt", v, sizeof(v))) {
    printf(" %s", v);
  }
  if (wpa_request(ifn, "SIGNAL_POLL", sig, sizeof(sig), 2000) > 0 && kv_line(sig, "RSSI", v, sizeof(v))) {
    printf(" %s dBm", v);
  }
  printf("\n");
}

static int w_scan(const char *ifn)
{
  wpa_conn_t c;
  char r[256];
  if (wpa_open(&c, ifn) != 0) {
    printf("wifi: %s: no wpa_supplicant control socket\n", ifn);
    return 1;
  }
  bool attached = wpa_cmd_on(&c, "ATTACH", r, sizeof(r), 2000) > 0 && strncmp(r, "OK", 2) == 0;
  if (wpa_cmd_on(&c, "SCAN", r, sizeof(r), 2000) <= 0) {
    printf("wifi: scan request not answered\n");
    wpa_close(&c);
    return 1;
  }
  bool busy = strncmp(r, "FAIL-BUSY", 9) == 0;
  if (!busy && strncmp(r, "OK", 2) != 0) {
    printf("wifi: scan refused (%s)\n", r);
    wpa_close(&c);
    return 1;
  }
  /* results event, at most 10 s; without ATTACH just wait the typical scan time */
  double end = mono_s() + 10.0;
  bool done = false;
  while (attached && !done && mono_s() < end) {
    int n = wpa_recv(&c, r, sizeof(r), (int)((end - mono_s()) * 1000.0) + 1, true);
    if (n <= 0) {
      break;
    }
    done = strstr(r, "CTRL-EVENT-SCAN-RESULTS") != NULL || strstr(r, "CTRL-EVENT-SCAN-FAILED") != NULL;
  }
  if (!attached) {
    sleep(5);
  }
  if (attached) {
    (void)wpa_cmd_on(&c, "DETACH", r, sizeof(r), 2000);
  }
  char *res = malloc(16384);
  int n = res != NULL ? wpa_cmd_on(&c, "SCAN_RESULTS", res, 16384, 3000) : -1;
  wpa_close(&c);
  if (n <= 0) {
    printf("wifi: no scan results\n");
    free(res);
    return 1;
  }
  printf("%s%s", res, (n > 0 && res[n - 1] != '\n') ? "\n" : "");
  free(res);
  return 0;
}

static int w_connect(const char *ssid, const char *psk)
{
  size_t sl = strlen(ssid), pl = psk != NULL ? strlen(psk) : 0;
  bool hex64 = pl == 64 && strspn(psk, "0123456789abcdefABCDEF") == 64;
  if (sl < 1 || sl > 32 || (pl != 0 && !hex64 && (pl < 8 || pl > 63))) {
    printf("wifi: invalid network (SSID 1..32 bytes, passphrase 8..63 characters or 64 hex)\n");
    return 1;
  }
  pthread_mutex_lock(&w_lock);
  snprintf(w_rt_ssid, sizeof(w_rt_ssid), "%s", ssid);
  snprintf(w_rt_psk, sizeof(w_rt_psk), "%s", psk != NULL ? psk : "");
  w_rt_set = true;
  pthread_mutex_unlock(&w_lock);
  char ifn[IFNAMSIZ], r[512];
  if (w_first_if(ifn, sizeof(ifn)) != 0) {
    printf("wifi: \"%s\" set; joined when a net80211 stick is attached\n", ssid);
    return 0;
  }
  if (w_conf(NULL) < 0) {
    printf("wifi: %s not written\n", WPA_CONF);
    return 1;
  }
  /* reload the networks, then select the new one (it has the highest priority anyway) */
  if (wpa_request(ifn, "RECONFIGURE", r, sizeof(r), 3000) <= 0) {
    printf("wifi: wpa_supplicant not answering\n");
    return 1;
  }
  if (wpa_request(ifn, "LIST_NETWORKS", r, sizeof(r), 2000) > 0) {
    /* "network id / ssid / bssid / flags" lines: the runtime network is written first = id 0 */
    (void)wpa_request(ifn, "SELECT_NETWORK 0", r, sizeof(r), 2000);
    (void)wpa_request(ifn, "ENABLE_NETWORK all", r, sizeof(r), 2000);
  }
  printf("wifi: joining \"%s\" on %s; 'wifi status' to follow\n", ssid, ifn);
  return 0;
}

int nx_wifi80211_cmd(int argc, char **argv)
{
  char ifn[IFNAMSIZ], r[64];
  const char *sub = argc >= 2 ? argv[1] : "status";
  if (strcmp(sub, "status") == 0) {
    pthread_mutex_lock(&w_lock);
    int n = w_running ? w_run_n : 0;
    char ifs[W_MAX][IFNAMSIZ];
    memcpy(ifs, w_run_set, sizeof(ifs));
    pthread_mutex_unlock(&w_lock);
    if (n == 0) {
      printf("wifi: net80211 stick %s, wpa_supplicant not running\n", w_parent_present ? "present" : "absent");
    }
    for (int i = 0; i < n; ++i) {
      w_status_line(ifs[i]);
    }
    return 0;
  }
  if (strcmp(sub, "scan") == 0) {
    if (w_first_if(ifn, sizeof(ifn)) != 0) {
      printf("wifi: no net80211 interface with wpa_supplicant\n");
      return 1;
    }
    return w_scan(ifn);
  }
  if ((strcmp(sub, "connect") == 0 || strcmp(sub, "join") == 0) && (argc == 3 || argc == 4)) {
    return w_connect(argv[2], argc == 4 ? argv[3] : NULL);
  }
  if (strcmp(sub, "disconnect") == 0 || strcmp(sub, "leave") == 0 || strcmp(sub, "reconnect") == 0) {
    bool dis = strcmp(sub, "reconnect") != 0;
    if (dis) {
      pthread_mutex_lock(&w_lock);
      w_rt_set = false;
      memset(w_rt_psk, 0, sizeof(w_rt_psk));
      pthread_mutex_unlock(&w_lock);
    }
    if (w_first_if(ifn, sizeof(ifn)) != 0) {
      printf("wifi: no net80211 interface with wpa_supplicant\n");
      return 1;
    }
    int rc = wpa_request(ifn, dis ? "DISCONNECT" : "REASSOCIATE", r, sizeof(r), 2000);
    printf("wifi: %s %s\n", ifn, rc > 0 ? (dis ? "disconnected (until 'wifi reconnect', a new stick or reboot)" : "reconnecting") : "no answer");
    return rc > 0 ? 0 : 1;
  }
  printf("usage (net80211 stick): wifi [status | scan | connect <ssid> [passphrase] | disconnect | reconnect]\n");
  return 1;
}

/* `wifi` when the AIC driver did not register it */
static int wifi_fallback_cmd(int argc, char **argv)
{
  return nx_wifi80211_cmd(argc, argv);
}

/* ---- config changes ---- */

static volatile int cfg_batch; /* net dhcp/static --save writes several keys: apply once */

static void on_config_change(void)
{
  w_reconf = true;
  if (!cfg_batch) {
    conf_apply(true);
  }
}

/* ---- `net` shell command ---- */

static void print_flags(int f)
{
  printf("%s%s%s%s", (f & IFF_UP) ? "UP" : "DOWN", (f & IFF_RUNNING) ? ",RUNNING" : "",
         (f & IFF_POINTOPOINT) ? ",P2P" : "", (f & IFF_LOOPBACK) ? ",LOOPBACK" : "");
}

/* default route: gateway and interface (routing table dump, NET_RT_DUMP) */
static int default_route(char *gw, size_t gcap, char *ifn, size_t icap)
{
  int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0};
  size_t len = 0;
  if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || len == 0) {
    return -1;
  }
  len += 512;
  char *buf = malloc(len);
  if (buf == NULL) {
    return -1;
  }
  int found = -1;
  if (sysctl(mib, 6, buf, &len, NULL, 0) == 0) {
    for (char *p = buf; p + sizeof(struct rt_msghdr) <= buf + len && found != 0;) {
      struct rt_msghdr *rtm = (struct rt_msghdr *)p;
      if (rtm->rtm_msglen == 0) {
        break;
      }
      struct sockaddr *sa = (struct sockaddr *)(rtm + 1);
      struct sockaddr *dst = NULL, *gate = NULL, *mask = NULL;
      for (int i = 0; i < RTAX_MAX; ++i) {
        if ((rtm->rtm_addrs & (1 << i)) == 0) {
          continue;
        }
        if (i == RTAX_DST) {
          dst = sa;
        } else if (i == RTAX_GATEWAY) {
          gate = sa;
        } else if (i == RTAX_NETMASK) {
          mask = sa;
        }
        size_t sl = sa->sa_len == 0 ? sizeof(long) : 1 + ((sa->sa_len - 1) | (sizeof(long) - 1));
        sa = (struct sockaddr *)((char *)sa + sl);
      }
      bool def = dst != NULL && dst->sa_family == AF_INET &&
                 ((struct sockaddr_in *)dst)->sin_addr.s_addr == INADDR_ANY &&
                 (mask == NULL || mask->sa_len == 0 || ((struct sockaddr_in *)mask)->sin_addr.s_addr == 0);
      if (def && gate != NULL && gate->sa_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)gate)->sin_addr, gw, (socklen_t)gcap);
        if (if_indextoname(rtm->rtm_index, ifn) == NULL) {
          snprintf(ifn, icap, "?");
        }
        found = 0;
      }
      p += rtm->rtm_msglen;
    }
  }
  free(buf);
  return found;
}

static void print_wifi_of(const char *ifn)
{
  if (strncmp(ifn, "wlan", 4) != 0) {
    return;
  }
  struct aicwf_wifi_status st;
  if (strcmp(ifn, AICWF_WIFI_IFNAME) == 0 && aicwf_wifi_status(&st) == 0 && st.station_ready) {
    if (st.connected) {
      printf("      wifi (AIC) \"%s\" channel %d %d dBm\n", st.ssid, st.channel, st.rssi);
    } else {
      printf("      wifi (AIC) %s\n", st.want_connected ? "connecting" : "no network set");
    }
    return;
  }
  char st2[1024], sig[256], v[80], s[48];
  if (wpa_request(ifn, "STATUS", st2, sizeof(st2), 1500) > 0) {
    printf("      wifi (net80211) %s", kv_line(st2, "wpa_state", v, sizeof(v)) ? v : "?");
    if (kv_line(st2, "ssid", s, sizeof(s))) {
      printf(" \"%s\"", s);
    }
    if (wpa_request(ifn, "SIGNAL_POLL", sig, sizeof(sig), 1500) > 0 && kv_line(sig, "RSSI", v, sizeof(v))) {
      printf(" %s dBm", v);
    }
    printf("\n");
  }
}

static int net_status(void)
{
  struct ifaddrs *ifa = NULL;
  if (getifaddrs(&ifa) != 0) {
    printf("net: getifaddrs failed (%d)\n", errno);
    return 1;
  }
  char seen[16][IFNAMSIZ];
  int nseen = 0;
  for (struct ifaddrs *i = ifa; i != NULL; i = i->ifa_next) {
    bool dup = false;
    for (int j = 0; j < nseen && !dup; ++j) {
      dup = strcmp(seen[j], i->ifa_name) == 0;
    }
    if (dup || nseen >= 16) {
      continue;
    }
    snprintf(seen[nseen++], IFNAMSIZ, "%s", i->ifa_name);
    int flags = (int)i->ifa_flags;
    printf("%-6s ", i->ifa_name);
    print_flags(flags);
    for (struct ifaddrs *k = ifa; k != NULL; k = k->ifa_next) {
      if (strcmp(k->ifa_name, i->ifa_name) != 0 || k->ifa_addr == NULL || k->ifa_addr->sa_family != AF_LINK) {
        continue;
      }
      struct sockaddr_dl *sdl = (struct sockaddr_dl *)k->ifa_addr;
      if (sdl->sdl_alen == 6) {
        const uint8_t *m = (const uint8_t *)LLADDR(sdl);
        printf(" ether %02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
      }
      if (k->ifa_data != NULL) {
        struct if_data *d = k->ifa_data;
        printf(" link %s mtu %u", d->ifi_link_state == LINK_STATE_UP ? "up" :
               d->ifi_link_state == LINK_STATE_DOWN ? "down" : "?", (unsigned)d->ifi_mtu);
      }
    }
    if (cfg_family(i->ifa_name) != 0) {
      nx_ifcfg_t c;
      pthread_mutex_lock(&cfg_lock);
      int mode = eff_cfg_locked(i->ifa_name, &c);
      pthread_mutex_unlock(&cfg_lock);
      printf(" [%s]", mode == NX_MODE_STATIC ? "static" : "dhcp");
    }
    printf("\n");
    for (struct ifaddrs *k = ifa; k != NULL; k = k->ifa_next) {
      if (strcmp(k->ifa_name, i->ifa_name) != 0 || k->ifa_addr == NULL || k->ifa_addr->sa_family != AF_INET) {
        continue;
      }
      char a[INET_ADDRSTRLEN], d[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &((struct sockaddr_in *)k->ifa_addr)->sin_addr, a, sizeof(a));
      int pfx = k->ifa_netmask != NULL ? mask_to_prefix(((struct sockaddr_in *)k->ifa_netmask)->sin_addr.s_addr) : 32;
      printf("      inet %s/%d", a, pfx);
      if ((flags & IFF_POINTOPOINT) && k->ifa_dstaddr != NULL && k->ifa_dstaddr->sa_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)k->ifa_dstaddr)->sin_addr, d, sizeof(d));
        printf(" -> %s", d);
      }
      printf("\n");
    }
    print_wifi_of(i->ifa_name);
  }
  freeifaddrs(ifa);
  char gw[INET_ADDRSTRLEN], gif[IFNAMSIZ];
  if (default_route(gw, sizeof(gw), gif, sizeof(gif)) == 0) {
    printf("gateway %s via %s\n", gw, gif);
  } else {
    printf("gateway none\n");
  }
  FILE *f = fopen("/etc/resolv.conf", "r");
  printf("dns");
  if (f != NULL) {
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
      char ns[64];
      if (sscanf(line, "nameserver %63s", ns) == 1) {
        printf(" %s", ns);
      }
    }
    fclose(f);
  }
  printf("\ndhcpcd %s", dhcpcd_state == 1 ? "running" : dhcpcd_state == 2 ? "ENDED" : "not started");
  if (dhcpcd_state == 2) {
    printf(" (exit %d, reboot to restore DHCP)", dhcpcd_exit);
  }
  printf("\n");
  return 0;
}

static int need_if(const char *ifn)
{
  if (if_nametoindex(ifn) == 0) {
    printf("net: no interface %s\n", ifn);
    return -1;
  }
  return 0;
}

static nx_ifcfg_t *ovr_slot_locked(const char *ifn)
{
  for (int i = 0; i < NX_OVR; ++i) {
    if (strcmp(ovr[i].ifn, ifn) == 0) {
      return &ovr[i];
    }
  }
  for (int i = 0; i < NX_OVR; ++i) {
    if (ovr[i].ifn[0] == '\0' || ovr[i].mode == NX_MODE_CFG) {
      memset(&ovr[i], 0, sizeof(ovr[i]));
      snprintf(ovr[i].ifn, sizeof(ovr[i].ifn), "%s", ifn);
      return &ovr[i];
    }
  }
  return NULL;
}

/* persist through the config store: only the documented keys exist for wlanN / ueN */
static int save_cfg(const char *ifn, const nx_ifcfg_t *c)
{
  int fam = cfg_family(ifn);
  if (fam == 0) {
    printf("net: no configuration key for %s (only wlanN and ueN are stored)\n", ifn);
    return -1;
  }
  char v[24];
  int rc = 0;
  if (c->mode == NX_MODE_STATIC) {
    snprintf(v, sizeof(v), "%s/%d", c->ip, c->prefix);
  } else {
    v[0] = '\0';
  }
  const char *gw = c->mode == NX_MODE_STATIC ? c->gw : "";
  if (fam == 1) {
    rc |= nexus_config_set("ipaddr_wlan", v);
    rc |= nexus_config_set("gateway_wlan", gw);
  } else {
    rc |= nexus_config_set("ipaddr_eth", v);
    rc |= nexus_config_set("netmask_eth", "");
    rc |= nexus_config_set("gateway_eth", gw);
  }
  if (c->mode == NX_MODE_STATIC && c->dns[0] != '\0') {
    rc |= nexus_config_set("dns", c->dns);
  }
  printf("net: %s settings %s\n", ifn, rc == 0 ? "saved" : "NOT saved (invalid value or storage error)");
  return rc == 0 ? 0 : -1;
}

static int net_addrmode(int argc, char **argv, bool is_static)
{
  bool save = false;
  int n = 0;
  char *pos[5];
  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--save") == 0) {
      save = true;
    } else if (n < 5) {
      pos[n++] = argv[i];
    }
  }
  if (n < 1 || (is_static && (n < 2 || n > 4)) || (!is_static && n != 1)) {
    printf("usage: net dhcp <if> [--save] | net static <if> <ip/prefix> [gw] [dns[,dns..]] [--save]\n");
    return 1;
  }
  const char *ifn = pos[0];
  if (strlen(ifn) >= IFNAMSIZ) {
    printf("net: bad interface name\n");
    return 1;
  }
  nx_ifcfg_t c;
  memset(&c, 0, sizeof(c));
  snprintf(c.ifn, sizeof(c.ifn), "%s", ifn);
  c.mode = is_static ? NX_MODE_STATIC : NX_MODE_DHCP;
  if (is_static) {
    struct in_addr t;
    if (parse_ip_prefix(pos[1], c.ip, sizeof(c.ip), &c.prefix, 24) != 0) {
      printf("net: address must be a.b.c.d[/prefix]\n");
      return 1;
    }
    if (n >= 3) {
      if (parse_ip4(pos[2], &t) != 0) {
        printf("net: bad gateway\n");
        return 1;
      }
      snprintf(c.gw, sizeof(c.gw), "%s", pos[2]);
    }
    if (n >= 4) {
      char tmp[64], *save_p = NULL;
      size_t o = 0;
      snprintf(tmp, sizeof(tmp), "%s", pos[3]);
      for (char *d = strtok_r(tmp, ",", &save_p); d != NULL; d = strtok_r(NULL, ",", &save_p)) {
        if (parse_ip4(d, &t) != 0 || o + strlen(d) + 2 > sizeof(c.dns)) {
          printf("net: bad DNS server %s\n", d);
          return 1;
        }
        o += (size_t)snprintf(c.dns + o, sizeof(c.dns) - o, "%s%s", o ? " " : "", d);
      }
    }
  }
  pthread_mutex_lock(&cfg_lock);
  nx_ifcfg_t *s = ovr_slot_locked(ifn);
  if (s != NULL) {
    if (save) {
      memset(s, 0, sizeof(*s)); /* the stored configuration is authoritative again */
    } else {
      *s = c;
    }
  }
  pthread_mutex_unlock(&cfg_lock);
  if (s == NULL) {
    printf("net: too many runtime overrides\n");
    return 1;
  }
  cfg_batch = 1;
  int saved = save ? save_cfg(ifn, &c) : 0;
  cfg_batch = 0;
  if (saved != 0) {
    pthread_mutex_lock(&cfg_lock);
    s = ovr_slot_locked(ifn);
    if (s != NULL) {
      *s = c; /* not stored: keep it for this boot */
    }
    pthread_mutex_unlock(&cfg_lock);
  }
  conf_apply(true);
  if (is_static) {
    printf("net: %s static %s/%d%s%s%s%s\n", ifn, c.ip, c.prefix, c.gw[0] ? " gw " : "", c.gw,
           c.dns[0] ? " dns " : "", c.dns);
  } else {
    printf("net: %s DHCP\n", ifn);
  }
  if (if_nametoindex(ifn) == 0) {
    printf("net: %s not present; applied when it appears\n", ifn);
  }
  return 0;
}

static int net_mac(int argc, char **argv)
{
  if (argc < 3 || need_if(argv[2]) != 0) {
    printf("usage: net mac <if> [xx:xx:xx:xx:xx:xx]\n");
    return 1;
  }
  if (argc == 3) {
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
      for (struct ifaddrs *i = ifa; i != NULL; i = i->ifa_next) {
        if (strcmp(i->ifa_name, argv[2]) == 0 && i->ifa_addr != NULL && i->ifa_addr->sa_family == AF_LINK) {
          struct sockaddr_dl *sdl = (struct sockaddr_dl *)i->ifa_addr;
          const uint8_t *m = (const uint8_t *)LLADDR(sdl);
          if (sdl->sdl_alen == 6) {
            printf("%s %02x:%02x:%02x:%02x:%02x:%02x\n", argv[2], m[0], m[1], m[2], m[3], m[4], m[5]);
          } else {
            printf("%s has no link-layer address\n", argv[2]);
          }
        }
      }
      freeifaddrs(ifa);
    }
    return 0;
  }
  unsigned b[6];
  char tail;
  if (sscanf(argv[3], "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail) != 6 ||
      (b[0] & 1) != 0) {
    printf("net: MAC must be xx:xx:xx:xx:xx:xx, unicast\n");
    return 1;
  }
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", argv[2]);
  ifr.ifr_addr.sa_len = 6;
  ifr.ifr_addr.sa_family = AF_LINK;
  for (int i = 0; i < 6; ++i) {
    ifr.ifr_addr.sa_data[i] = (char)b[i];
  }
  int s = if_sock();
  int rc = s >= 0 ? ioctl(s, SIOCSIFLLADDR, &ifr) : -1;
  int e = errno;
  if (s >= 0) {
    close(s);
  }
  if (rc != 0) {
    printf("net: %s: MAC not changed (%s)\n", argv[2], strerror(e));
    return 1;
  }
  /* the kernel announces the addresses with the new MAC (arp_iflladdr); dhcpcd learns it on
   * the next carrier-up: bounce so the lease is requested with the new MAC (until reboot) */
  (void)if_bounce(argv[2]);
  printf("net: %s MAC set (runtime only)\n", argv[2]);
  return 0;
}

static int resolve(const char *host, const char *port, int socktype, struct addrinfo **res)
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = socktype;
  int rc = getaddrinfo(host, port, &hints, res);
  if (rc != 0) {
    printf("net: %s: %s\n", host, gai_strerror(rc));
    return -1;
  }
  return 0;
}

/* printable copy of received bytes */
static void print_data(const char *pfx, const uint8_t *d, ssize_t n)
{
  printf("%s(%d) ", pfx, (int)n);
  for (ssize_t i = 0; i < n; ++i) {
    if (d[i] == '\n') {
      printf("\\n");
    } else if (d[i] == '\r') {
      printf("\\r");
    } else if (isprint(d[i])) {
      putchar(d[i]);
    } else {
      printf("\\x%02x", d[i]);
    }
  }
  printf("\n");
}

static int clamp_secs(const char *s, int dflt)
{
  int v = s != NULL ? atoi(s) : dflt;
  return v < 1 ? 1 : (v > 3600 ? 3600 : v);
}

static int net_udp(int argc, char **argv)
{
  if (argc >= 6 && strcmp(argv[2], "send") == 0) {
    struct addrinfo *res = NULL;
    if (resolve(argv[3], argv[4], SOCK_DGRAM, &res) != 0) {
      return 1;
    }
    char text[512];
    size_t o = 0;
    for (int i = 5; i < argc && o < sizeof(text) - 1; ++i) {
      o += (size_t)snprintf(text + o, sizeof(text) - o, "%s%s", i > 5 ? " " : "", argv[i]);
    }
    o = o < sizeof(text) ? o : sizeof(text) - 1;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    ssize_t n = s >= 0 ? sendto(s, text, o, 0, res->ai_addr, res->ai_addrlen) : -1;
    printf("net: udp %zd bytes to %s:%s%s\n", n, argv[3], argv[4], n < 0 ? " FAILED" : "");
    if (n >= 0) {
      /* one answer within 1 s (an echo service), no retransmission: UDP */
      struct pollfd p = {.fd = s, .events = POLLIN};
      if (poll(&p, 1, 1000) > 0) {
        uint8_t r[512];
        ssize_t k = recv(s, r, sizeof(r), 0);
        if (k >= 0) {
          print_data("reply ", r, k);
        }
      }
    }
    if (s >= 0) {
      close(s);
    }
    freeaddrinfo(res);
    return n < 0;
  }
  if (argc >= 4 && strcmp(argv[2], "listen") == 0) {
    int port = atoi(argv[3]), secs = clamp_secs(argc > 4 ? argv[4] : NULL, 10);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_len = sizeof(a), .sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (s < 0 || port < 1 || port > 65535 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
      printf("net: cannot bind udp port %d (%s)\n", port, strerror(errno));
      if (s >= 0) {
        close(s);
      }
      return 1;
    }
    printf("net: udp listening on %d for %d s\n", port, secs);
    double end = mono_s() + secs;
    int count = 0;
    while (mono_s() < end) {
      struct pollfd p = {.fd = s, .events = POLLIN};
      if (poll(&p, 1, (int)((end - mono_s()) * 1000.0) + 1) <= 0) {
        continue;
      }
      uint8_t buf[1500];
      struct sockaddr_in from;
      socklen_t fl = sizeof(from);
      ssize_t n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
      if (n >= 0) {
        char f[INET_ADDRSTRLEN];
        char pfx[48];
        inet_ntop(AF_INET, &from.sin_addr, f, sizeof(f));
        snprintf(pfx, sizeof(pfx), "%s:%u ", f, ntohs(from.sin_port));
        print_data(pfx, buf, n);
        ++count;
      }
    }
    close(s);
    printf("net: %d datagram(s)\n", count);
    return 0;
  }
  printf("usage: net udp send <host> <port> <text> | net udp listen <port> [secs]\n");
  return 1;
}

/* read and print from a connected socket until close, idle_ms of silence or the deadline */
static void tcp_drain(int c, int idle_ms, double end)
{
  uint8_t buf[512];
  while (mono_s() < end) {
    struct pollfd p = {.fd = c, .events = POLLIN};
    int left = (int)((end - mono_s()) * 1000.0) + 1;
    if (poll(&p, 1, left < idle_ms ? left : idle_ms) <= 0) {
      break;
    }
    ssize_t n = recv(c, buf, sizeof(buf), 0);
    if (n <= 0) {
      printf("net: %s\n", n == 0 ? "peer closed" : strerror(errno));
      break;
    }
    print_data("", buf, n);
  }
}

static int net_tcp(int argc, char **argv)
{
  if (argc >= 5 && strcmp(argv[2], "connect") == 0) {
    struct addrinfo *res = NULL;
    if (resolve(argv[3], argv[4], SOCK_STREAM, &res) != 0) {
      return 1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
      freeaddrinfo(res);
      return 1;
    }
    /* non-blocking connect with a 5 s limit, then blocking I/O with timeouts */
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    double t0 = mono_s();
    int rc = connect(s, res->ai_addr, res->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      struct pollfd p = {.fd = s, .events = POLLOUT};
      int err = ETIMEDOUT;
      socklen_t el = sizeof(err);
      if (poll(&p, 1, 5000) > 0) {
        err = 0;
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
      }
      rc = err == 0 ? 0 : -1;
      errno = err;
    }
    freeaddrinfo(res);
    if (rc != 0) {
      printf("net: tcp connect %s:%s failed (%s)\n", argv[3], argv[4], strerror(errno));
      close(s);
      return 1;
    }
    fcntl(s, F_SETFL, fl);
    struct timeval tv = {.tv_sec = 3};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    printf("net: tcp connected to %s:%s in %.0f ms\n", argv[3], argv[4], (mono_s() - t0) * 1000.0);
    if (argc >= 6) {
      char text[512];
      size_t o = 0;
      for (int i = 5; i < argc && o < sizeof(text) - 2; ++i) {
        o += (size_t)snprintf(text + o, sizeof(text) - o, "%s%s", i > 5 ? " " : "", argv[i]);
      }
      o = o < sizeof(text) - 2 ? o : sizeof(text) - 3;
      text[o++] = '\r';
      text[o++] = '\n';
      if (send(s, text, o, 0) < 0) {
        printf("net: send failed (%s)\n", strerror(errno));
      }
    }
    tcp_drain(s, 2000, mono_s() + 10.0);
    close(s);
    return 0;
  }
  if (argc >= 4 && strcmp(argv[2], "listen") == 0) {
    int port = atoi(argv[3]), secs = clamp_secs(argc > 4 ? argv[4] : NULL, 30), one = 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_len = sizeof(a), .sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (s >= 0) {
      setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    if (s < 0 || port < 1 || port > 65535 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 1) != 0) {
      printf("net: cannot listen on tcp port %d (%s)\n", port, strerror(errno));
      if (s >= 0) {
        close(s);
      }
      return 1;
    }
    printf("net: tcp listening on %d for %d s\n", port, secs);
    double end = mono_s() + secs;
    while (mono_s() < end) {
      struct pollfd p = {.fd = s, .events = POLLIN};
      if (poll(&p, 1, (int)((end - mono_s()) * 1000.0) + 1) <= 0) {
        continue;
      }
      struct sockaddr_in from;
      socklen_t fl = sizeof(from);
      int c = accept(s, (struct sockaddr *)&from, &fl);
      if (c < 0) {
        continue;
      }
      char f[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &from.sin_addr, f, sizeof(f));
      printf("net: connection from %s:%u\n", f, ntohs(from.sin_port));
      struct timeval tv = {.tv_sec = 3};
      setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      tcp_drain(c, 10000, end);
      close(c);
    }
    close(s);
    return 0;
  }
  printf("usage: net tcp connect <host> <port> [text] | net tcp listen <port> [secs]\n");
  return 1;
}

static int net_dns(const char *name)
{
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  double t0 = mono_s();
  int rc = getaddrinfo(name, NULL, &hints, &res);
  double ms = (mono_s() - t0) * 1000.0;
  if (rc != 0) {
    printf("net: %s: %s (%.0f ms)\n", name, gai_strerror(rc), ms);
    return 1;
  }
  for (struct addrinfo *a = res; a != NULL; a = a->ai_next) {
    char t[INET6_ADDRSTRLEN] = "?";
    if (a->ai_family == AF_INET) {
      inet_ntop(AF_INET, &((struct sockaddr_in *)a->ai_addr)->sin_addr, t, sizeof(t));
    } else if (a->ai_family == AF_INET6) {
      inet_ntop(AF_INET6, &((struct sockaddr_in6 *)a->ai_addr)->sin6_addr, t, sizeof(t));
    }
    printf("%s %s\n", name, t);
  }
  printf("(%.0f ms)\n", ms);
  freeaddrinfo(res);
  return 0;
}

/*
 * tun (if_tuntap.c): L3 point-to-point, one packet per read()/write(), no header by default
 * (TUNSIFHEAD off: writes are taken as IPv4). On RTEMS /dev/tunN appears when the interface is
 * created (SIOCIFCREATE2; opening a missing /dev/tunN does not clone it), and closing the
 * device takes the interface down and purges its addresses (tundtor): the owner opens first,
 * then sets the addresses, after every open (or `net tun create` again after opening).
 */
static int net_tun(int argc, char **argv)
{
  if (argc >= 6 && strcmp(argv[2], "create") == 0) {
    int unit = atoi(argv[3]), mtu = argc > 6 ? atoi(argv[6]) : 1500;
    struct in_addr local, peer;
    char ifn[IFNAMSIZ];
    if (unit < 0 || unit > 3 || parse_ip4(argv[4], &local) != 0 || parse_ip4(argv[5], &peer) != 0 ||
        mtu < 68 || mtu > 9000) {
      printf("usage: net tun create <0..3> <local ip> <peer ip> [mtu]\n");
      return 1;
    }
    snprintf(ifn, sizeof(ifn), "tun%d", unit);
    int s = if_sock();
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifn);
    if (if_nametoindex(ifn) == 0 && (s < 0 || ioctl(s, SIOCIFCREATE2, &ifr) != 0)) {
      printf("net: %s not created (%s)\n", ifn, strerror(errno));
      if (s >= 0) {
        close(s);
      }
      return 1;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifn);
    ifr.ifr_mtu = mtu;
    if (s >= 0 && ioctl(s, SIOCSIFMTU, &ifr) != 0) {
      printf("net: %s mtu %d not set (%s)\n", ifn, mtu, strerror(errno));
    }
    if (s >= 0) {
      close(s);
    }
    (void)if_del_inet(ifn);
    if (if_add_inet(ifn, local, 32, &peer) != 0 || if_set_up(ifn, true) != 0) {
      printf("net: %s address not set (%s)\n", ifn, strerror(errno));
      return 1;
    }
    printf("net: %s %s -> %s mtu %d; open /dev/%s\n", ifn, argv[4], argv[5], mtu, ifn);
    return 0;
  }
  if (argc >= 4 && strcmp(argv[2], "destroy") == 0) {
    char ifn[IFNAMSIZ];
    snprintf(ifn, sizeof(ifn), "tun%d", atoi(argv[3]));
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifn);
    int s = if_sock();
    int rc = s >= 0 ? ioctl(s, SIOCIFDESTROY, &ifr) : -1;
    printf("net: %s %s\n", ifn, rc == 0 ? "destroyed" : strerror(errno));
    if (s >= 0) {
      close(s);
    }
    return rc != 0;
  }
  /* status */
  int shown = 0;
  for (int u = 0; u < 4; ++u) {
    char ifn[IFNAMSIZ], dev[24];
    int f;
    struct stat st;
    snprintf(ifn, sizeof(ifn), "tun%d", u);
    snprintf(dev, sizeof(dev), "/dev/%s", ifn);
    if (if_nametoindex(ifn) == 0 || if_get_flags(ifn, &f) != 0) {
      continue;
    }
    ++shown;
    printf("%s ", ifn);
    print_flags(f);
    printf(" %s %s\n", dev, stat(dev, &st) == 0 ? "present" : "missing");
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
      for (struct ifaddrs *k = ifa; k != NULL; k = k->ifa_next) {
        if (strcmp(k->ifa_name, ifn) == 0 && k->ifa_addr != NULL && k->ifa_addr->sa_family == AF_INET) {
          char a[INET_ADDRSTRLEN], d[INET_ADDRSTRLEN] = "-";
          inet_ntop(AF_INET, &((struct sockaddr_in *)k->ifa_addr)->sin_addr, a, sizeof(a));
          if (k->ifa_dstaddr != NULL) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)k->ifa_dstaddr)->sin_addr, d, sizeof(d));
          }
          printf("      inet %s -> %s\n", a, d);
        }
      }
      freeifaddrs(ifa);
    }
  }
  if (shown == 0) {
    printf("net: no tun interface (net tun create <0..3> <local> <peer> [mtu])\n");
  }
  printf("closing /dev/tunN takes it down and purges its addresses: set them after each open\n");
  return 0;
}

static void net_usage(void)
{
  printf("usage: net status\n"
         "       net dhcp <if> [--save] | net static <if> <ip/prefix> [gw] [dns,..] [--save]\n"
         "       net release <if> | net renew <if> | net up <if> | net down <if>\n"
         "       net mac <if> [xx:xx:xx:xx:xx:xx] | net arp [flush]\n"
         "       net ping <host> [count] | net dns <name> | net sockets\n"
         "       net udp send <host> <port> <text> | net udp listen <port> [secs]\n"
         "       net tcp connect <host> <port> [text] | net tcp listen <port> [secs]\n"
         "       net tun [create <0..3> <local> <peer> [mtu] | destroy <0..3>]\n");
}

static int net_cmd(int argc, char **argv)
{
  const char *sub = argc >= 2 ? argv[1] : "status";
  if (strcmp(sub, "status") == 0) {
    return net_status();
  }
  if (strcmp(sub, "dhcp") == 0 || strcmp(sub, "static") == 0) {
    return net_addrmode(argc, argv, strcmp(sub, "static") == 0);
  }
  if ((strcmp(sub, "release") == 0 || strcmp(sub, "renew") == 0 || strcmp(sub, "up") == 0 ||
       strcmp(sub, "down") == 0) && argc == 3) {
    if (need_if(argv[2]) != 0) {
      return 1;
    }
    int rc;
    if (strcmp(sub, "release") == 0) {
      char lease[64];
      snprintf(lease, sizeof(lease), DHCPCD_LEASE, argv[2]);
      (void)unlink(lease);
      rc = if_del_inet(argv[2]) >= 0 ? 0 : -1;
      printf("net: %s addresses removed (no DHCPRELEASE sent: dhcpcd has no control socket on "
             "RTEMS); 'net renew %s' for a new lease\n", argv[2], argv[2]);
    } else if (strcmp(sub, "renew") == 0) {
      rc = if_bounce(argv[2]);
      printf("net: %s restarted (dhcpcd re-reads its settings and asks for a lease)\n", argv[2]);
    } else {
      rc = if_set_up(argv[2], strcmp(sub, "up") == 0);
    }
    if (rc != 0) {
      printf("net: %s: %s\n", argv[2], strerror(errno));
    }
    return rc != 0;
  }
  if (strcmp(sub, "mac") == 0) {
    return net_mac(argc, argv);
  }
  if (strcmp(sub, "arp") == 0) {
    if (argc >= 3 && strcmp(argv[2], "flush") == 0) {
      char *av[] = {"arp", "-d", "-a", NULL};
      return bsd_prog(rtems_bsd_command_arp, 3, av);
    }
    char *av[] = {"arp", "-an", NULL};
    return bsd_prog(rtems_bsd_command_arp, 2, av);
  }
  if (strcmp(sub, "ping") == 0 && argc >= 3) {
    char cnt[8];
    int c = argc > 3 ? atoi(argv[3]) : 4;
    snprintf(cnt, sizeof(cnt), "%d", c < 1 ? 1 : (c > 100 ? 100 : c));
    char *av[] = {"ping", "-c", cnt, "-W", "1000", argv[2], NULL};
    return bsd_prog(rtems_bsd_command_ping, 6, av);
  }
  if (strcmp(sub, "sockets") == 0) {
    char *av[] = {"netstat", "-an", NULL};
    return bsd_prog(rtems_bsd_command_netstat, 2, av);
  }
  if (strcmp(sub, "dns") == 0 && argc == 3) {
    return net_dns(argv[2]);
  }
  if (strcmp(sub, "udp") == 0) {
    return net_udp(argc, argv);
  }
  if (strcmp(sub, "tcp") == 0) {
    return net_tcp(argc, argv);
  }
  if (strcmp(sub, "tun") == 0) {
    return net_tun(argc, argv);
  }
  net_usage();
  return 1;
}

/* ---- start ---- */

static void sysctl_int(const char *name, int v)
{
  if (sysctlbyname(name, NULL, NULL, &v, sizeof(v)) != 0) {
    printf("net: sysctl %s not set (%d)\n", name, errno);
  }
}

void nx_net_start(void)
{
  pthread_mutex_lock(&cfg_lock);
  cfg_loaded = true;
  pthread_mutex_unlock(&cfg_lock);
  /* interfaces that came up before the configuration was read got DHCP: re-apply */
  conf_apply(true);
  nexus_config_on_change(on_config_change);
  /* one gratuitous ARP per new address is sent by the kernel; repeat it twice (1 s, 2 s) so a
   * swapped adapter (new MAC, same address) is learned even if the first one is lost */
  sysctl_int("net.link.ether.inet.garp_rexmit_count", 2);
  /* SO_KEEPALIVE sockets (SSH; FreeBSD's tcp always_keepalive covers the others): a dead
   * peer after an adapter swap is found in ~2 min instead of 2 h */
  sysctl_int("net.inet.tcp.keepidle", 60000);
  sysctl_int("net.inet.tcp.keepintvl", 10000);
  /* wpa_supplicant SCAN_RESULTS replies over PF_UNIX datagrams */
  {
    unsigned long v = 16384;
    if (sysctlbyname("net.local.dgram.maxdgram", NULL, NULL, &v, sizeof(v)) != 0) {
      printf("net: sysctl net.local.dgram.maxdgram not set (%d)\n", errno);
    }
  }
  rtems_shell_add_cmd("net", "network",
                      "net status|dhcp|static|release|renew|up|down|mac|arp|ping|dns|sockets|udp|tcp|tun",
                      net_cmd);
}
