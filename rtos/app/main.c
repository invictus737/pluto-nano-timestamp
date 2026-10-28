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
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#include <rtems/score/smpimpl.h>

#include <bsp.h>

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
      continue;
    }
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

static void start_dhcpcd(void)
{
  static const char cfg[] = "clientid nexus-rtos\nbackground\noption domain_name_servers\n";
  int fd = open("/etc/dhcpcd.conf", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd >= 0) {
    (void)write(fd, cfg, sizeof(cfg) - 1);
    close(fd);
  }
  rtems_status_code sc = rtems_dhcpcd_start(NULL);
  if (sc != RTEMS_SUCCESSFUL) {
    printf("dhcpcd: start failed (%d)\n", (int)sc);
  }
}

static void *net_watch(void *arg)
{
  (void)arg;
  for (;;) {
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
    sleep(1);
  }
  return NULL;
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

  rtems_bsd_setlogpriority("info");
  rtems_status_code sc = rtems_bsd_initialize();
  assert(sc == RTEMS_SUCCESSFUL);
  rtems_task_wake_after(2);
  rtems_bsd_ifconfig_lo0();
  start_dhcpcd();
  printf("bsd: up at %.3f s\n", uptime_s());

  spawn(net_watch, 5, 8192);
  spawn(http_status, 5, 16384);
#ifdef NEXUS_RUST
  spawn(rust_entry, 5, 256 * 1024);
#endif

  sc = rtems_shell_init("SHLL", 32 * 1024, 1, CONSOLE_DEVICE_NAME, false, true, NULL);
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
#endif

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
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 64
#define CONFIGURE_MAXIMUM_USER_EXTENSIONS 1
#define CONFIGURE_UNLIMITED_OBJECTS
#define CONFIGURE_UNIFIED_WORK_AREAS
#define CONFIGURE_STACK_CHECKER_ENABLED
#define CONFIGURE_MICROSECONDS_PER_TICK 1000

#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_STACK_SIZE (32 * 1024)
#define CONFIGURE_INIT_TASK_INITIAL_MODES RTEMS_DEFAULT_MODES
#define CONFIGURE_INIT_TASK_ATTRIBUTES RTEMS_FLOATING_POINT

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

#define CONFIGURE_SHELL_USER_COMMANDS \
  SHELL_WLANSTATS_COMMAND \
  SHELL_WPA_SUPPLICANT_COMMAND \
  &bsp_interrupt_shell_command, \
  &rtems_shell_ARP_Command, \
  &rtems_shell_HOSTNAME_Command, \
  &rtems_shell_PING_Command, \
  &rtems_shell_ROUTE_Command, \
  &rtems_shell_NETSTAT_Command, \
  &rtems_shell_IFCONFIG_Command, \
  &rtems_shell_SYSCTL_Command, \
  &rtems_shell_VMSTAT_Command

#define CONFIGURE_SHELL_COMMAND_CPUINFO
#define CONFIGURE_SHELL_COMMAND_CPUUSE
#define CONFIGURE_SHELL_COMMAND_STACKUSE
#define CONFIGURE_SHELL_COMMAND_PERIODUSE
#define CONFIGURE_SHELL_COMMAND_MALLOC_INFO
#define CONFIGURE_SHELL_COMMAND_LS
#define CONFIGURE_SHELL_COMMAND_CAT
#define CONFIGURE_SHELL_COMMAND_RESET
#define CONFIGURE_SHELL_COMMAND_SHUTDOWN

#include <rtems/shellconfig.h>
