/*
 * Nexus-BS RTOS for the Pluto Nano, milestones M1/M2: boot banner with the real PS clocks,
 * both Cortex-A9 cores, heartbeat LED, USB host (Zynq EHCI) with USB-Ethernet / USB Wi-Fi,
 * DHCP, a plain-text HTTP status page on port 80 and the RTEMS shell on UART1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <machine/rtems-bsd-commands.h>
#include <rtems.h>
#include <rtems/bsd/bsd.h>
#include <rtems/console.h>
#include <rtems/cpuuse.h>
#include <rtems/dhcpcd.h>
#include <rtems/malloc.h>
#include <rtems/printer.h>
#include <rtems/shell.h>
#include <rtems/version.h>
#include <rtems/score/smpimpl.h>

#include <bsp.h>

#include "config_store.h"
#include "netcmd.h"
#include "ssh_server.h"

int nexus_wifi_start(const char *ssid, const char *psk);
static void wifi80211_start(const char *ssid, const char *psk);

#define NEXUS_RTOS_VERSION "0.1.0-m2"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* PS clocks read back from the SLCR (platform.c) */
uint64_t nexus_arm_hz(void);
uint64_t nexus_uart_ref_hz(void);
#define arm_hz nexus_arm_hz
#define uart_ref_hz nexus_uart_ref_hz

/* PL configured? devcfg INT_STS PCFG_DONE */
#define DEVCFG_INT_STS 0xf800700cU
static int pl_done(void)
{
  return (REG32(DEVCFG_INT_STS) >> 2) & 1U;
}

/* ---- heartbeat LED: Nano LED on MIO 15 (GPIO bank 0, bit 15) ---- */

#define GPIO_MASK_DATA_0_LSW 0xe000a000U
#define GPIO_DIRM_0 0xe000a204U
#define GPIO_OEN_0 0xe000a208U
#define LED_BIT 15

static void led_set(int on)
{
  uint32_t bit = 1U << LED_BIT;
  REG32(GPIO_MASK_DATA_0_LSW) = ((~bit & 0xffffU) << 16) | (on ? bit : 0);
}

static void *heartbeat(void *arg)
{
  (void)arg;
  REG32(GPIO_DIRM_0) |= 1U << LED_BIT;
  REG32(GPIO_OEN_0) |= 1U << LED_BIT;
  for (int on = 1;; on = !on) {
    led_set(on);
    usleep(on ? 100000 : 900000);
  }
  return NULL;
}

/* ---- uptime ---- */

static double uptime_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static volatile double t_net_up = -1.0;

/* ---- HTTP status (plain text, one request per connection, low priority) ---- */

static int status_text(char *buf, size_t cap)
{
  int n = 0;
  struct ifaddrs *ifa = NULL;
  Heap_Information_block heap;

  n += snprintf(buf + n, cap - n,
                "Nexus-BS RTOS %s (RTEMS %s) on Pluto Nano\n"
                "uptime %.3f s, network up at %.3f s\n"
                "cpus %u, arm %llu Hz, uart ref %llu Hz, PL %s\n",
                NEXUS_RTOS_VERSION, rtems_version(), uptime_s(), t_net_up,
                rtems_scheduler_get_processor_maximum(),
                (unsigned long long)arm_hz(), (unsigned long long)uart_ref_hz(),
                pl_done() ? "configured" : "empty");
  if (malloc_info(&heap) == 0) {
    n += snprintf(buf + n, cap - n, "heap free %lu B, largest %lu B, used %lu B\n",
                  (unsigned long)heap.Free.total, (unsigned long)heap.Free.largest,
                  (unsigned long)heap.Used.total);
  }
  if (getifaddrs(&ifa) == 0) {
    for (struct ifaddrs *i = ifa; i != NULL && n < (int)cap - 80; i = i->ifa_next) {
      if (i->ifa_addr != NULL && i->ifa_addr->sa_family == AF_INET) {
        char a[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof(a));
        n += snprintf(buf + n, cap - n, "if %s inet %s%s\n", i->ifa_name, a,
                      (i->ifa_flags & IFF_UP) ? " up" : "");
      }
    }
    freeifaddrs(ifa);
  }
  return n;
}

static void *http_status(void *arg)
{
  (void)arg;
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  struct sockaddr_in a = {.sin_len = sizeof(a), .sin_family = AF_INET, .sin_port = htons(80)};
  static char body[2048];
  static char head[160];

  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
    printf("http: cannot listen on :80\n");
    return NULL;
  }
  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) {
      if (errno != EINTR) {
        sleep(1); /* EMFILE/ENOBUFS: do not spin at the accept */
      }
      continue;
    }
    /* a client that connects and sends nothing must not hold the page forever */
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    char req[256];
    (void)read(c, req, sizeof(req)); /* request line ignored: one page */
    int n = status_text(body, sizeof(body));
    int h = snprintf(head, sizeof(head),
                     "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    (void)write(c, head, h);
    (void)write(c, body, n);
    close(c);
  }
  return NULL;
}

/* ---- network bring-up: interfaces arrive by USB hotplug, dhcpcd follows them ---- */

/*
 * DNS and NTP servers from DHCP. On RTEMS dhcpcd runs no hook scripts (dhcpcd/script.c: only
 * rtems_dhcpcd_add_hook() handlers are called), so nothing writes /etc/resolv.conf and the
 * libbsd resolver would only try 127.0.0.1: every host name lookup (nexus2 networks, SNTP)
 * failed. The resolver re-reads the file when its mtime changes (res_state.c, every 2 s).
 */
#define NET_IFS 4
static struct {
  char ifname[16];
  char dns[128];   /* space separated */
  char domain[64];
  char ntp[128];
} net_if[NET_IFS];
static pthread_mutex_t net_if_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *env_get(char *const *env, const char *name)
{
  size_t n = strlen(name);
  for (; *env != NULL; ++env) {
    if (strncmp(*env, name, n) == 0 && (*env)[n] == '=') {
      return *env + n + 1;
    }
  }
  return NULL;
}

static void resolv_conf_write_locked(void)
{
  char buf[640];
  size_t o = 0;
  for (int i = 0; i < NET_IFS && o < sizeof(buf) - 80; ++i) {
    if (net_if[i].ifname[0] != '\0' && net_if[i].domain[0] != '\0') {
      o += (size_t)snprintf(buf + o, sizeof(buf) - o, "search %s\n", net_if[i].domain);
      break;
    }
  }
  for (int i = -1; i < NET_IFS; ++i) {
    char tmp[128];
    char *save = NULL;
    /* -1: [NETWORK] dns of config.txt, ahead of the DHCP servers */
    const char *cfg = i < 0 ? nexus_config_get("dns") : NULL;
    snprintf(tmp, sizeof(tmp), "%s", i < 0 ? (cfg != NULL ? cfg : "") : net_if[i].dns);
    for (char *t = strtok_r(tmp, " ", &save); t != NULL && o < sizeof(buf) - 64;
         t = strtok_r(NULL, " ", &save)) {
      o += (size_t)snprintf(buf + o, sizeof(buf) - o, "nameserver %s\n", t);
    }
  }
  if (o == 0) {
    return; /* keep the last servers while no interface has a lease */
  }
  /* 2 s per server and 2 rounds (default 5 s x 2): a server behind a lost link costs less */
  if (o < sizeof(buf) - 32) {
    o += (size_t)snprintf(buf + o, sizeof(buf) - o, "options timeout:2 attempts:2\n");
  }
  int fd = open("/etc/resolv.conf", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd >= 0) {
    (void)write(fd, buf, o);
    close(fd);
  }
}

static void dhcpcd_hook_handler(rtems_dhcpcd_hook *hook, char *const *env)
{
  (void)hook;
  const char *ifname = env_get(env, "interface");
  const char *reason = env_get(env, "reason");
  if (ifname == NULL || reason == NULL) {
    return;
  }
  int bound = strcmp(reason, "BOUND") == 0 || strcmp(reason, "RENEW") == 0 ||
              strcmp(reason, "REBIND") == 0 || strcmp(reason, "REBOOT") == 0 ||
              strcmp(reason, "INFORM") == 0 || strcmp(reason, "STATIC") == 0;
  /* NOCARRIER too: a dead link's servers would cost every lookup a resolver timeout */
  int gone = strcmp(reason, "EXPIRE") == 0 || strcmp(reason, "RELEASE") == 0 ||
             strcmp(reason, "STOP") == 0 || strcmp(reason, "DEPARTED") == 0 ||
             strcmp(reason, "NOCARRIER") == 0;
  if (!bound && !gone) {
    return;
  }
  pthread_mutex_lock(&net_if_lock);
  int slot = -1;
  for (int i = 0; i < NET_IFS && slot < 0; ++i) {
    if (strcmp(net_if[i].ifname, ifname) == 0) {
      slot = i;
    }
  }
  for (int i = 0; i < NET_IFS && slot < 0 && bound; ++i) {
    if (net_if[i].ifname[0] == '\0') {
      slot = i;
    }
  }
  if (slot >= 0) {
    if (bound) {
      const char *dns = env_get(env, "new_domain_name_servers");
      const char *dom = env_get(env, "new_domain_name");
      const char *ntp = env_get(env, "new_ntp_servers");
      if (dns == NULL) {
        dns = env_get(env, "new_routers"); /* home routers answer DNS themselves */
      }
      snprintf(net_if[slot].ifname, sizeof(net_if[slot].ifname), "%s", ifname);
      snprintf(net_if[slot].dns, sizeof(net_if[slot].dns), "%s", dns ? dns : "");
      snprintf(net_if[slot].domain, sizeof(net_if[slot].domain), "%s", dom ? dom : "");
      snprintf(net_if[slot].ntp, sizeof(net_if[slot].ntp), "%s", ntp ? ntp : "");
      printf("net: %s %s, dns %s%s%s\n", ifname, reason, dns ? dns : "none",
             ntp ? ", ntp " : "", ntp ? ntp : "");
    } else {
      memset(&net_if[slot], 0, sizeof(net_if[slot]));
    }
    resolv_conf_write_locked();
  }
  pthread_mutex_unlock(&net_if_lock);
}

static rtems_dhcpcd_hook dhcpcd_hook = {.name = "nexus-dns", .handler = dhcpcd_hook_handler};

/*
 * Wall clock. RTEMS starts at 1970 and nothing else sets it (libbsd has no NTP daemon), so
 * every TLS certificate (rustls in nexus2) would be "not yet valid". At boot the clock is
 * raised to the build date (certificates in use were issued before the build, most are still
 * valid after it); SNTP then sets the real time as soon as an interface has an address.
 */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static void clock_floor_build_date(void)
{
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *date = __DATE__; /* "Mmm dd yyyy"; SOURCE_DATE_EPOCH keeps builds reproducible */
  char mon[4] = {date[0], date[1], date[2], 0};
  const char *mp = strstr(months, mon);
  int day = atoi(date + 4), year = atoi(date + 7);
  struct timespec now;
  if (mp == NULL || day < 1 || year < 2024) {
    return;
  }
  time_t floor_s = (time_t)(days_from_civil(year, (unsigned)((mp - months) / 3 + 1), (unsigned)day) * 86400);
  clock_gettime(CLOCK_REALTIME, &now);
  if (now.tv_sec < floor_s) {
    struct timespec ts = {.tv_sec = floor_s, .tv_nsec = 0};
    (void)clock_settime(CLOCK_REALTIME, &ts);
  }
}

#define NTP_UNIX_OFFSET 2208988800u

static int sntp_query(const struct sockaddr *sa, socklen_t salen, struct timespec *out)
{
  uint8_t pkt[48];
  struct timespec t1, t4;
  int s = socket(sa->sa_family, SOCK_DGRAM, 0);
  if (s < 0) {
    return -1;
  }
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0x23; /* LI 0, version 4, client */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  /* transmit timestamp = a nonce the server echoes as origin */
  uint32_t nonce_hi = (uint32_t)t1.tv_nsec ^ 0x5a17c0deu, nonce_lo = (uint32_t)t1.tv_sec * 2654435761u;
  for (int i = 0; i < 4; ++i) {
    pkt[40 + i] = (uint8_t)(nonce_hi >> (24 - 8 * i));
    pkt[44 + i] = (uint8_t)(nonce_lo >> (24 - 8 * i));
  }
  int rc = -1;
  if (sendto(s, pkt, sizeof(pkt), 0, sa, salen) == (ssize_t)sizeof(pkt)) {
    uint8_t r[68];
    for (int tries = 0; tries < 3; ++tries) {
      ssize_t n = recv(s, r, sizeof(r), 0);
      if (n < 48) {
        break;
      }
      clock_gettime(CLOCK_MONOTONIC, &t4);
      if ((r[0] & 7) != 4 || r[1] == 0 || r[1] > 15 || memcmp(r + 24, pkt + 40, 8) != 0) {
        continue; /* not a server answer to this request (or kiss-of-death) */
      }
      uint32_t sec = ((uint32_t)r[40] << 24) | ((uint32_t)r[41] << 16) | ((uint32_t)r[42] << 8) | r[43];
      uint32_t frac = ((uint32_t)r[44] << 24) | ((uint32_t)r[45] << 16) | ((uint32_t)r[46] << 8) | r[47];
      /* era 0 ends in 2036: values below the offset belong to era 1 */
      int64_t unix_s = sec >= NTP_UNIX_OFFSET ? (int64_t)sec - NTP_UNIX_OFFSET
                                              : (int64_t)sec + 4294967296LL - NTP_UNIX_OFFSET;
      int64_t ns = unix_s * 1000000000LL + (int64_t)(((uint64_t)frac * 1000000000ULL) >> 32);
      int64_t rtt = ((int64_t)t4.tv_sec - t1.tv_sec) * 1000000000LL + (t4.tv_nsec - t1.tv_nsec);
      ns += rtt / 2;
      out->tv_sec = (time_t)(ns / 1000000000LL);
      out->tv_nsec = (long)(ns % 1000000000LL);
      rc = 0;
      break;
    }
  }
  close(s);
  return rc;
}

static int sntp_host(const char *host)
{
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(host, "123", &hints, &res) != 0 || res == NULL) {
    return -1;
  }
  int rc = -1;
  for (struct addrinfo *a = res; a != NULL && rc != 0; a = a->ai_next) {
    struct timespec ts;
    if (sntp_query(a->ai_addr, a->ai_addrlen, &ts) == 0 && ts.tv_sec > 1700000000) {
      struct timespec before;
      clock_gettime(CLOCK_REALTIME, &before);
      if (clock_settime(CLOCK_REALTIME, &ts) == 0) {
        char a_s[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &((struct sockaddr_in *)a->ai_addr)->sin_addr, a_s, sizeof(a_s));
        printf("time: set from %s (%s), step %+.3f s\n", host, a_s,
               (double)(ts.tv_sec - before.tv_sec) + (ts.tv_nsec - before.tv_nsec) * 1e-9);
        rc = 0;
      }
    }
  }
  freeaddrinfo(res);
  return rc;
}

static void *sntp_main(void *arg)
{
  (void)arg;
  unsigned backoff = 1;
  for (;;) {
    if (t_net_up < 0) {
      usleep(250000); /* no address yet */
      continue;
    }
    char servers[192];
    size_t o = 0;
    const char *cfg = nexus_config_get("ntp_server"); /* optional key, kept by the config store */
    if (cfg != NULL && cfg[0] != '\0') {
      o += (size_t)snprintf(servers + o, sizeof(servers) - o, "%s ", cfg);
    }
    pthread_mutex_lock(&net_if_lock);
    for (int i = 0; i < NET_IFS && o < sizeof(servers); ++i) {
      if (net_if[i].ntp[0] != '\0') {
        o += (size_t)snprintf(servers + o, sizeof(servers) - o, "%s ", net_if[i].ntp);
      }
    }
    pthread_mutex_unlock(&net_if_lock);
    if (o < sizeof(servers)) {
      /* last: a public pool, and an address for when DNS does not answer */
      snprintf(servers + o, sizeof(servers) - o, "pool.ntp.org time.cloudflare.com 162.159.200.1");
    }
    int ok = 0;
    char *save = NULL;
    for (char *h = strtok_r(servers, " ", &save); h != NULL && !ok; h = strtok_r(NULL, " ", &save)) {
      ok = sntp_host(h) == 0;
    }
    if (ok) {
      backoff = 1;
      sleep(3600);
    } else {
      sleep(backoff);
      backoff = backoff < 64 ? backoff * 2 : 64;
    }
  }
  return NULL;
}

static void start_dhcpcd(void)
{
  /* /etc/dhcpcd.conf from netcmd.c: fast lease (no ARP collision probe; the kernel still sends
   * a gratuitous ARP for every new address), IPv4 only, DNS and NTP servers to
   * dhcpcd_hook_handler, the host name to the DHCP server, and per interface either DHCP or the
   * static [WLAN]/[USB_ETHERNET] address (dhcpcd applies it itself, also on hotplug). Before
   * nexus_config_init() it holds DHCP for all; nx_net_start() re-applies after the config. */
  rtems_dhcpcd_add_hook(&dhcpcd_hook);
  nx_net_dhcpcd_conf_write();
  rtems_status_code sc = rtems_dhcpcd_start(nx_net_dhcpcd_config());
  if (sc != RTEMS_SUCCESSFUL) {
    printf("dhcpcd: start failed (%d)\n", (int)sc);
  }
}

static void *net_watch(void *arg)
{
  (void)arg;
  while (t_net_up < 0) {
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
      for (struct ifaddrs *i = ifa; i != NULL; i = i->ifa_next) {
        if (i->ifa_addr != NULL && i->ifa_addr->sa_family == AF_INET &&
            strcmp(i->ifa_name, "lo0") != 0 && t_net_up < 0) {
          char a[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof(a));
          t_net_up = uptime_s();
          printf("net: %s %s at %.3f s\n", i->ifa_name, a, t_net_up);
        }
      }
      freeifaddrs(ifa);
    }
    if (t_net_up < 0) {
      usleep(200000);
    }
  }
  return NULL; /* first address seen: SNTP and the status page have their time stamp */
}

#ifdef NEXUS_RUST
int nexus_rust_main(void);

static void *rust_entry(void *arg)
{
  (void)arg;
  printf("rust: start at %.3f s\n", uptime_s());
  int rc = nexus_rust_main();
  printf("rust: main returned %d at %.3f s\n", rc, uptime_s());
  return NULL;
}
#endif

static void spawn(void *(*fn)(void *), int prio, size_t stack);

int nexus_shell_abi(int argc, char **argv); /* abi.c */

#ifdef NEXUS_RUST_MAIN
/* ---- nexus2 station: Rust main() in its own thread, radio service threads pinned to CPU1 ---- */

int main(int argc, char **argv); /* nexus2 (Rust) */
int nexus2_request_stop(void);   /* bins/nexus2/src/rtems_platform.rs */
int pluto_rtems_pin_to_cpu(int cpu);

#define NEXUS2_CONFIG "/data/nexus2/nexus2.toml"

static volatile int nexus2_running;

void nexus_exit_guard(int on, void (*cb)(int status)); /* posix_compat.c */

static void nexus2_exited(int status)
{
  nexus2_running = 0;
  printf("nexus2: exited with %d\n", status);
}

static void *nexus2_entry(void *arg)
{
  (void)arg;
  /* RTEMS starts with an empty environment; nexus2 needs an absolute HOME for its RF state
   * (live_run.rs rf_state_path) */
  if (getenv("HOME") == NULL) {
    (void)setenv("HOME", "/data/nexus2", 1);
  }
  (void)mkdir("/data/nexus2", 0755);
  /* a start error calls exit(): end this thread, not the system */
  nexus_exit_guard(1, nexus2_exited);
  static char a0[] = "nexus2", a1[] = "--config", a2[] = NEXUS2_CONFIG, a3[] = "run", a4[] = "--live",
              a5[] = "--allow-rf-tx";
  static char *argv[] = {a0, a1, a2, a3, a4, a5, NULL};
  nexus2_running = 1;
  printf("nexus2: starting (%s) at %.3f s\n", NEXUS2_CONFIG, uptime_s());
  int rc = main(6, argv);
  nexus_exit_guard(0, NULL);
  nexus2_exited(rc);
  return NULL;
}

static void nexus2_start(void)
{
  struct stat st;
  if (nexus2_running) {
    printf("nexus2: already running\n");
    return;
  }
  if (stat(NEXUS2_CONFIG, &st) != 0) {
    printf("nexus2: no %s yet: put nexus2.toml on a USB stick (rtos/config/README.md)\n", NEXUS2_CONFIG);
    return;
  }
  /* radio DMA service threads and interrupts on CPU1, network/USB/SSH stay on CPU0 */
  (void)pluto_rtems_pin_to_cpu(1);
  spawn(nexus2_entry, 4, 2 * 1024 * 1024);
}

static int shell_nexus(int argc, char **argv)
{
  if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
    printf("nexus2: %s\n", nexus2_request_stop() == 0 ? "stop requested" : "not running");
  } else if (argc >= 2 && strcmp(argv[1], "start") == 0) {
    nexus2_start();
  } else {
    printf("nexus2: %s\nusage: nexus start|stop|status\n", nexus2_running ? "running" : "stopped");
  }
  return 0;
}
#endif

static void spawn(void *(*fn)(void *), int prio, size_t stack)
{
  pthread_attr_t at;
  struct sched_param sp = {.sched_priority = prio};
  pthread_t t;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, stack);
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&at, SCHED_FIFO);
  pthread_attr_setschedparam(&at, &sp);
  int rc = pthread_create(&t, &at, fn, NULL);
  assert(rc == 0);
  pthread_attr_destroy(&at);
}

static void Init(rtems_task_argument arg)
{
  (void)arg;
  printf("\n\nNexus-BS RTOS %s (RTEMS %s) Pluto Nano\n", NEXUS_RTOS_VERSION, rtems_version());
  printf("boot %.3f s, cpus %u, arm %llu Hz, uart ref %llu Hz, PL %s\n", uptime_s(),
         rtems_scheduler_get_processor_maximum(), (unsigned long long)arm_hz(),
         (unsigned long long)uart_ref_hz(), pl_done() ? "configured" : "empty");

  spawn(heartbeat, 10, 4096);
  clock_floor_build_date();

  /* the USB config.txt/nexus2.toml drop-in: the media server must listen before USB attaches
   * a stick at boot; the drop-in itself waits for /data (nexus_config_init below) */
  nexus_config_early();

  rtems_bsd_setlogpriority("info");
  rtems_status_code sc = rtems_bsd_initialize();
  assert(sc == RTEMS_SUCCESSFUL);
  rtems_task_wake_after(2);
  rtems_bsd_ifconfig_lo0();
  (void)sethostname("pluto", 5);
  start_dhcpcd();
  printf("bsd: up at %.3f s\n", uptime_s());

  /* /data (QSPI JFFS2 mount: polled flash reads) while USB enumerates the Wi-Fi stick */
  if (nexus_config_init() != 0) {
    printf("config: init failed, running with defaults\n");
  }
  {
    const char *hn = nexus_config_get("hostname"); /* [NETWORK] hostname, sent with DHCP */
    if (hn != NULL && hn[0] != '\0' && strlen(hn) <= 63) {
      (void)sethostname(hn, strlen(hn));
    }
  }
  /* ---- netcmd: static addresses from the config, `net` shell command (netcmd.c) ---- */
  nx_net_start();
  /* ---- end netcmd ---- */

  /* Wi-Fi: first configured network (config.txt / build default); the driver keeps the
   * station connected across link loss and replug, dhcpcd follows wlan0 */
  {
    const char *ssid = NULL, *psk = NULL;
    if (nexus_config_wifi(0, &ssid, &psk) != 0 || ssid == NULL || ssid[0] == '\0') {
      ssid = NULL;
    }
    (void)nexus_wifi_start(ssid, (psk != NULL && psk[0] != '\0') ? psk : NULL);
    wifi80211_start(ssid, (psk != NULL && psk[0] != '\0') ? psk : NULL);
  }
  if (nexus_ssh_start() != 0) {
    printf("ssh: not started\n");
  }
  nexus_config_shell_register();
  rtems_shell_add_cmd("abi", "misc", "abi [-v]: C layouts/constants vs the Rust libc crate", nexus_shell_abi);
#ifdef NEXUS_RUST_MAIN
  rtems_shell_add_cmd("nexus", "misc", "nexus start|stop|status: the nexus2 station", shell_nexus);
  nexus2_start();
#endif

  spawn(net_watch, 5, 8192);
  spawn(sntp_main, 5, 16384);
  spawn(http_status, 5, 16384);
#ifdef NEXUS_RUST
  spawn(rust_entry, 5, 256 * 1024);
#endif

  /* classic priority 1 would be the most urgent task in the system: a busy shell command (cp,
   * hexdump, mdump) would preempt libbsd, USB and the radio DMA service threads. 200 = the SSH
   * shells. */
  sc = rtems_shell_init("SHLL", 32 * 1024, 200, CONSOLE_DEVICE_NAME, false, true, NULL);
  assert(sc == RTEMS_SUCCESSFUL);
}

/* ---- libbsd: own nexus device table (the Nano has no GEM Ethernet and no SD card) ---- */

#include <machine/rtems-bsd-nexus-bus.h>
#include <rtems/bsd/modules.h>

RTEMS_BSD_DRIVER_XILINX_ZYNQ_SLCR;
RTEMS_BSD_DEFINE_NEXUS_DEVICE(ehci, 0, 0, NULL);
SYSINIT_DRIVER_REFERENCE(usbus, ehci);
RTEMS_BSD_DRIVER_USB;
RTEMS_BSD_DRIVER_USB_MASS;
/* AIC8800 Wi-Fi/BT sticks: leave the driver-CD storage mode before umass claims them */
SYSINIT_DRIVER_REFERENCE(aicms, uhub);
/* AIC8800 Wi-Fi/BT: USB interface and wlan0 ifnet attachment for the RT-Smart driver */
SYSINIT_DRIVER_REFERENCE(aicwf, uhub);
SYSINIT_DRIVER_REFERENCE(axe, uhub);
SYSINIT_DRIVER_REFERENCE(axge, uhub);
SYSINIT_DRIVER_REFERENCE(ure, uhub);
SYSINIT_DRIVER_REFERENCE(cdce, uhub);
RTEMS_BSD_DRIVER_UKPHY;
RTEMS_BSD_DRIVER_REPHY;
#ifdef RTEMS_BSD_MODULE_NET80211
SYSINIT_DRIVER_REFERENCE(rtwn_usb, uhub);
SYSINIT_MODULE_REFERENCE(wlan_ratectl_none);
SYSINIT_MODULE_REFERENCE(wlan_sta);
SYSINIT_MODULE_REFERENCE(wlan_amrr);
SYSINIT_MODULE_REFERENCE(wlan_wep);
SYSINIT_MODULE_REFERENCE(wlan_tkip);
SYSINIT_MODULE_REFERENCE(wlan_ccmp);
SYSINIT_REFERENCE(rtwn_rtl8188eufw);
SYSINIT_REFERENCE(rtwn_rtl8192cfwT);
/* the other drivers of the dev_usb_wlan set: Ralink RT2870/3070/5370 (run, firmware below),
 * RT2501/RT73 (rum), RT2500 (ural), ZyDAS ZD1211 (zyd), Realtek RTL8187 (urtw) */
SYSINIT_DRIVER_REFERENCE(run, uhub);
SYSINIT_REFERENCE(runfw);
SYSINIT_DRIVER_REFERENCE(rum, uhub);
SYSINIT_DRIVER_REFERENCE(ural, uhub);
SYSINIT_DRIVER_REFERENCE(zyd, uhub);
SYSINIT_DRIVER_REFERENCE(urtw, uhub);
#endif

/*
 * net80211 sticks (rtwn, run, rum, ural, zyd, urtw): the AIC driver brings its own station,
 * these need a wlan clone and wpa_supplicant. netcmd.c supervises both across hotplug (wlan1 /
 * wlan2 per stick, one supplicant instance restarted when the set of sticks changes).
 */
static void wifi80211_start(const char *ssid, const char *psk)
{
  nx_wifi80211_start(ssid, psk);
}

#define RTEMS_BSD_CONFIG_NET_PF_UNIX
#define RTEMS_BSD_CONFIG_INIT
#include <machine/rtems-bsd-config.h>

/* ---- RTEMS configuration ---- */

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_STUB_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_ZERO_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_LIBBLOCK

#define CONFIGURE_MAXIMUM_PROCESSORS 2
/* CPU1 is optional: if the BootROM parking loop is not there, boot on one core. */
#define CONFIGURE_SCHEDULER_EDF_SMP
#include <rtems/scheduler.h>
RTEMS_SCHEDULER_EDF_SMP(main);
#define CONFIGURE_SCHEDULER_TABLE_ENTRIES \
  RTEMS_SCHEDULER_TABLE_EDF_SMP(main, rtems_build_name('M', 'A', 'I', 'N'))
#define CONFIGURE_SCHEDULER_ASSIGNMENTS \
  RTEMS_SCHEDULER_ASSIGN(0, RTEMS_SCHEDULER_ASSIGN_PROCESSOR_MANDATORY), \
  RTEMS_SCHEDULER_ASSIGN(0, RTEMS_SCHEDULER_ASSIGN_PROCESSOR_OPTIONAL)
/* console 3, libbsd (dhcpcd route/bpf/udp per interface), SSH (3 x 3 pty streams + 3 per
 * session), HTTP, dosfs/jffs2 files and every nexus2 socket (networks, dashboard and its
 * WebSocket clients) share this table; 64 was close to exhausted. Code that uses select()
 * must not see descriptors >= FD_SETSIZE (newlib: 64): the SSH server uses poll(). */
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 256
#define CONFIGURE_MAXIMUM_USER_EXTENSIONS 1
#define CONFIGURE_UNLIMITED_OBJECTS
#define CONFIGURE_UNIFIED_WORK_AREAS
#define CONFIGURE_STACK_CHECKER_ENABLED
#define CONFIGURE_MICROSECONDS_PER_TICK 1000

#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_STACK_SIZE (32 * 1024)
#define CONFIGURE_INIT_TASK_INITIAL_MODES RTEMS_DEFAULT_MODES
#define CONFIGURE_INIT_TASK_ATTRIBUTES RTEMS_FLOATING_POINT

void nexus_fatal_extension(rtems_fatal_source source, bool always_set_to_false, rtems_fatal_code code);
#define CONFIGURE_INITIAL_EXTENSIONS {.fatal = nexus_fatal_extension}

#define CONFIGURE_INIT
#include <rtems/confdefs.h>

#define CONFIGURE_SHELL_COMMANDS_INIT
#include <bsp/irq-info.h>
#include <rtems/netcmds-config.h>

#ifdef RTEMS_BSD_MODULE_USR_SBIN_WPA_SUPPLICANT
#define SHELL_WPA_SUPPLICANT_COMMAND &rtems_shell_WPA_SUPPLICANT_Command,
#else
#define SHELL_WPA_SUPPLICANT_COMMAND
#endif
#ifdef RTEMS_BSD_MODULE_USER_SPACE_WLANSTATS
#define SHELL_WLANSTATS_COMMAND &rtems_shell_WLANSTATS_Command,
#else
#define SHELL_WLANSTATS_COMMAND
#endif

/*
 * libbsd programs parse their arguments with newlib's global getopt, whose state survives the
 * call: the second `ifconfig wlan0 create ...` in a shell session was parsed from the wrong
 * index. Every libbsd command goes through a wrapper that reinitialises getopt (optind = 0).
 */
#define NX_GETOPT_SAFE(ID, ORIG)                                                   \
  static int nx_sh_##ID(int argc, char **argv)                                    \
  {                                                                               \
    optind = 0;                                                                   \
    return ORIG.command(argc, argv);                                              \
  }                                                                               \
  static rtems_shell_cmd_t nx_cmd_##ID = {.name = #ID, .topic = "net", .command = nx_sh_##ID}
extern rtems_shell_cmd_t rtems_shell_ARP_Command, rtems_shell_PING_Command,
    rtems_shell_ROUTE_Command, rtems_shell_NETSTAT_Command, rtems_shell_IFCONFIG_Command,
    rtems_shell_SYSCTL_Command, rtems_shell_VMSTAT_Command;
NX_GETOPT_SAFE(arp, rtems_shell_ARP_Command);
NX_GETOPT_SAFE(ping, rtems_shell_PING_Command);
NX_GETOPT_SAFE(route, rtems_shell_ROUTE_Command);
NX_GETOPT_SAFE(netstat, rtems_shell_NETSTAT_Command);
NX_GETOPT_SAFE(ifconfig, rtems_shell_IFCONFIG_Command);
NX_GETOPT_SAFE(sysctl, rtems_shell_SYSCTL_Command);
NX_GETOPT_SAFE(vmstat, rtems_shell_VMSTAT_Command);
#ifdef RTEMS_BSD_MODULE_USR_SBIN_WPA_SUPPLICANT
extern rtems_shell_cmd_t rtems_shell_WPA_SUPPLICANT_Command;
NX_GETOPT_SAFE(wpa_supplicant, rtems_shell_WPA_SUPPLICANT_Command);
#undef SHELL_WPA_SUPPLICANT_COMMAND
#define SHELL_WPA_SUPPLICANT_COMMAND &nx_cmd_wpa_supplicant,
#endif
#ifdef RTEMS_BSD_MODULE_USER_SPACE_WLANSTATS
extern rtems_shell_cmd_t rtems_shell_WLANSTATS_Command;
NX_GETOPT_SAFE(wlanstats, rtems_shell_WLANSTATS_Command);
#undef SHELL_WLANSTATS_COMMAND
#define SHELL_WLANSTATS_COMMAND &nx_cmd_wlanstats,
#endif

#define CONFIGURE_SHELL_USER_COMMANDS \
  SHELL_WLANSTATS_COMMAND \
  SHELL_WPA_SUPPLICANT_COMMAND \
  &bsp_interrupt_shell_command, \
  &nx_cmd_arp, \
  &rtems_shell_HOSTNAME_Command, \
  &nx_cmd_ping, \
  &nx_cmd_route, \
  &nx_cmd_netstat, \
  &nx_cmd_ifconfig, \
  &nx_cmd_sysctl, \
  &nx_cmd_vmstat

#define CONFIGURE_SHELL_COMMAND_CPUINFO
#define CONFIGURE_SHELL_COMMAND_CPUUSE
#define CONFIGURE_SHELL_COMMAND_STACKUSE
#define CONFIGURE_SHELL_COMMAND_PERIODUSE
#define CONFIGURE_SHELL_COMMAND_MALLOC_INFO
#define CONFIGURE_SHELL_COMMAND_WKSPACE_INFO
#define CONFIGURE_SHELL_COMMAND_TOP
#define CONFIGURE_SHELL_COMMAND_RTEMS
#define CONFIGURE_SHELL_COMMAND_DF
#define CONFIGURE_SHELL_COMMAND_MOUNT
#define CONFIGURE_SHELL_COMMAND_UNMOUNT
#define CONFIGURE_SHELL_COMMAND_DATE
#define CONFIGURE_SHELL_COMMAND_ECHO
#define CONFIGURE_SHELL_COMMAND_PWD
#define CONFIGURE_SHELL_COMMAND_CD
#define CONFIGURE_SHELL_COMMAND_MKDIR
#define CONFIGURE_SHELL_COMMAND_RM
#define CONFIGURE_SHELL_COMMAND_CP
#define CONFIGURE_SHELL_COMMAND_MV
#define CONFIGURE_SHELL_COMMAND_HEXDUMP
#define CONFIGURE_SHELL_COMMAND_MDUMP
#define CONFIGURE_SHELL_COMMAND_WDUMP
#define CONFIGURE_SHELL_COMMAND_MEDIT
#define CONFIGURE_SHELL_COMMAND_LS
#define CONFIGURE_SHELL_COMMAND_CAT
#define CONFIGURE_SHELL_COMMAND_RESET
#define CONFIGURE_SHELL_COMMAND_SHUTDOWN

#include <rtems/shellconfig.h>
