/*
 * Host test of the USB Ethernet glue (rtos/cherryusb/net/usbnet.c) against fakes of the
 * CherryUSB class drivers and the libbsd ifnet (fake_usbnet.c): attach/detach, link
 * up/down, frame integrity both ways (RTL8152 FCS trim, transmit padding), interface naming
 * with two adapters, the configuration choice, and a hotplug soak with traffic in flight.
 * Run under AddressSanitizer (run.sh): use-after-free, leaks and double detach abort.
 *
 * usage: test_usbnet [cycles]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "usbh_core.h"
#include "usbnet.h"
#include "fake_usbnet.h"

static int g_fail;
#define CHECK(c, ...)                                                    \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);        \
            printf(__VA_ARGS__);                                         \
            printf("\n");                                                \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

static void msleep(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_link(const char *ifn, int want, unsigned ms)
{
    for (unsigned t = 0; t < ms; t++) {
        if (fake_if_link(ifn) == want) {
            return 0;
        }
        msleep(1);
    }
    return -1;
}

static int wait_rx_drained(int k, unsigned ms)
{
    for (unsigned t = 0; t < ms; t++) {
        if (fake_rx_pending(k) == 0) {
            msleep(2); /* the last frame through the glue */
            return 0;
        }
        msleep(1);
    }
    return -1;
}

static int wait_tx(unsigned long want, unsigned ms)
{
    struct fake_if_stats st;
    for (unsigned t = 0; t < ms; t++) {
        fake_if_stats(&st);
        if (st.tx_ok + st.tx_err >= want) {
            return 0;
        }
        msleep(1);
    }
    return -1;
}

static const uint32_t lens[] = { 60, 64, 504, 505, 508, 511, 512, 513, 1016, 1020, 1024, 1500, 1514 };
#define NLENS (sizeof(lens) / sizeof(lens[0]))

/* plug, link up, traffic both ways, link down/up, unplug */
static void t_basic(int k, const char *name)
{
    struct fake_if_stats a, b;
    struct fake_dev_stats d0, d;
    char st[512];

    printf("basic %s\n", name);
    fake_if_stats(&a);
    fake_dev_stats(k, &d0);
    fake_plug(k);
    CHECK(fake_if_count() == 1, "interfaces %d", fake_if_count());
    CHECK(fake_if_link("ue0") != -2, "no ue0");
    fake_cable(k, true);
    CHECK(wait_link("ue0", 1, 1500) == 0, "link up not reported");

    for (uint32_t i = 0; i < 200; i++) {
        while (fake_rx_push(k, lens[i % NLENS], i) != 0) {
            msleep(1);
        }
    }
    CHECK(wait_rx_drained(k, 2000) == 0, "rx not drained");
    for (uint32_t i = 0; i < 200; i++) {
        while (fake_if_transmit("ue0", lens[i % NLENS], i) != 0) {
            msleep(1);
        }
    }
    CHECK(wait_tx(a.tx_ok + a.tx_err + 200, 2000) == 0, "tx not done");
    fake_if_stats(&b);
    fake_dev_stats(k, &d);
    CHECK(b.rx_ok - a.rx_ok == 200 && b.rx_bad == a.rx_bad, "rx ok %lu bad %lu", b.rx_ok - a.rx_ok, b.rx_bad - a.rx_bad);
    CHECK(d.tx_ok - d0.tx_ok == 200, "tx ok %u", d.tx_ok - d0.tx_ok);
    CHECK(d.tx_bad_pad == d0.tx_bad_pad, "transfers that need a ZLP: %u", d.tx_bad_pad - d0.tx_bad_pad);
    CHECK(d.tx_bad_data == d0.tx_bad_data, "corrupt tx: %u", d.tx_bad_data - d0.tx_bad_data);
    CHECK(d.rx_threads_alive == 1, "rx threads %u", d.rx_threads_alive);

    nexus_usbnet_status(st, sizeof(st));
    CHECK(strstr(st, "ue0") != NULL && strstr(st, "link up") != NULL, "status: %s", st);

    fake_cable(k, false);
    CHECK(wait_link("ue0", 0, 1500) == 0, "link down not reported");
    fake_if_transmit("ue0", 100, 1);
    msleep(20);
    fake_cable(k, true);
    CHECK(wait_link("ue0", 1, 1500) == 0, "link up again not reported");

    fake_unplug(k);
    CHECK(fake_if_count() == 0, "interfaces after unplug %d", fake_if_count());
    CHECK(fake_wait_rx_gone(k, 1000) == 0, "rx thread did not end");
    nexus_usbnet_status(st, sizeof(st));
    CHECK(strstr(st, "ue0") == NULL, "status after unplug: %s", st);
}

/* polled chips: one link-down read (shorter than two poll periods) is not reported */
static void t_debounce(void)
{
    struct fake_if_stats a, b;

    printf("rtl8152 link-down debounce\n");
    fake_plug(FK_RTL);
    fake_cable(FK_RTL, true);
    CHECK(wait_link("ue0", 1, 1500) == 0, "link up not reported");
    fake_if_stats(&a);
    for (int i = 0; i < 5; i++) {
        msleep(300);
        fake_cable(FK_RTL, false);
        msleep(150); /* at most one poll inside */
        fake_cable(FK_RTL, true);
    }
    msleep(600);
    fake_if_stats(&b);
    CHECK(b.link_downs == a.link_downs, "%lu false link-down(s) reported", b.link_downs - a.link_downs);
    fake_cable(FK_RTL, false);
    CHECK(wait_link("ue0", 0, 2500) == 0, "real link down not reported");
    fake_unplug(FK_RTL);
    CHECK(fake_wait_rx_gone(FK_RTL, 1000) == 0, "rx thread did not end");
}

/* two adapters: distinct names, a freed unit is reused */
static void t_two(void)
{
    printf("two adapters\n");
    fake_plug(FK_RTL);
    fake_plug(FK_ECM);
    CHECK(fake_if_link("ue0") != -2 && fake_if_link("ue1") != -2, "ue0/ue1 missing");
    fake_unplug(FK_RTL);
    CHECK(fake_if_link("ue0") == -2 && fake_if_link("ue1") != -2, "ue0 not freed");
    fake_plug(FK_ASIX);
    CHECK(fake_if_link("ue0") != -2, "ue0 not reused");
    fake_unplug(FK_ECM);
    fake_unplug(FK_ASIX);
    CHECK(fake_if_count() == 0, "interfaces %d", fake_if_count());
    CHECK(fake_wait_rx_gone(FK_RTL, 1000) == 0 && fake_wait_rx_gone(FK_ECM, 1000) == 0 &&
          fake_wait_rx_gone(FK_ASIX, 1000) == 0, "rx threads left");
}

/* ---- hotplug soak with traffic ---- */

static volatile int g_soak_run;
static volatile int g_soak_dev;

static void *soak_traffic(void *a)
{
    uint32_t seq = 0;
    (void)a;
    while (g_soak_run) {
        (void)fake_if_transmit("ue0", lens[seq % NLENS], seq);
        (void)fake_rx_push(g_soak_dev, lens[(seq * 7) % NLENS], seq);
        if ((++seq & 15) == 0) {
            msleep(1);
        }
    }
    return NULL;
}

static void t_soak(unsigned cycles)
{
    pthread_t t;
    struct fake_if_stats a, b;

    printf("hotplug soak, %u cycles with traffic\n", cycles);
    fake_if_stats(&a);
    srand(7);
    g_soak_run = 1;
    g_soak_dev = FK_ECM;
    pthread_create(&t, NULL, soak_traffic, NULL);
    for (unsigned c = 0; c < cycles; c++) {
        int k = (c & 1) ? FK_RTL : FK_ECM;
        g_soak_dev = k;
        fake_plug(k);
        if (rand() % 4 != 0) {
            fake_cable(k, true);
        }
        msleep((unsigned)(rand() % 15));
        if (rand() % 3 == 0) {
            fake_cable(k, false);
            msleep((unsigned)(rand() % 3));
        }
        fake_unplug(k);
        if (rand() % 2) {
            msleep((unsigned)(rand() % 3)); /* else replug at once: old rx thread may still run */
        }
    }
    g_soak_run = 0;
    pthread_join(t, NULL);
    fake_if_stats(&b);
    for (int k = 0; k < FK_N; k++) {
        struct fake_dev_stats d;
        CHECK(fake_wait_rx_gone(k, 2000) == 0, "device %d: rx threads left", k);
        fake_dev_stats(k, &d);
        CHECK(d.rx_threads_max <= 1, "device %d: %u rx threads at once", k, d.rx_threads_max);
        CHECK(d.tx_bad_pad == 0 && d.tx_bad_data == 0, "device %d: tx pad %u data %u", k, d.tx_bad_pad, d.tx_bad_data);
    }
    CHECK(fake_if_count() == 0, "interfaces left %d", fake_if_count());
    CHECK(b.attaches - a.attaches == cycles && b.detaches - a.detaches == cycles, "attach %lu detach %lu",
          b.attaches - a.attaches, b.detaches - a.detaches);
    CHECK(b.rx_bad == a.rx_bad, "bad rx frames %lu", b.rx_bad - a.rx_bad);
    printf("  rx %lu tx %lu (refused/err %lu) link up %lu down %lu\n", b.rx_ok - a.rx_ok, b.tx_ok - a.tx_ok,
           b.tx_err - a.tx_err, b.link_ups - a.link_ups, b.link_downs - a.link_downs);
}

/* ---- configuration choice ---- */

uint8_t usbh_get_hport_active_config_index(struct usbh_hubport *hport);

#define CFG(n_if) 9, 2, (uint8_t)(9 + 9 * (n_if)), 0, n_if, 1, 0, 0x80, 50
#define IF(num, c, s, p) 9, 4, num, 0, 0, c, s, p, 0

static void t_cfgsel(void)
{
    static const uint8_t vendor[] = { CFG(1), IF(0, 0xff, 0xff, 0x00) };
    static const uint8_t ecm[] = { CFG(2), IF(0, 0x02, 0x06, 0x00), IF(1, 0x0a, 0x00, 0x00) };
    static const uint8_t ncm[] = { CFG(2), IF(0, 0x02, 0x0d, 0x00), IF(1, 0x0a, 0x00, 0x00) };
    struct usbh_hubport *hp = fake_hport(FK_ECM);

    printf("configuration choice\n");
    {
        const uint8_t *d[] = { vendor, ecm };
        const uint16_t l[] = { sizeof(vendor), sizeof(ecm) };
        fake_set_configs(FK_ECM, 0x0bda, 0x8153, 2, d, l); /* RTL8153: vendor mode unsupported */
        CHECK(usbh_get_hport_active_config_index(hp) == 1, "RTL8153 should use CDC-ECM");
        fake_set_configs(FK_ECM, 0x0bda, 0x8152, 2, d, l); /* RTL8152: vendor driver */
        CHECK(usbh_get_hport_active_config_index(hp) == 0, "RTL8152 should stay vendor");
    }
    {
        const uint8_t *d[] = { vendor, ncm, ecm };
        const uint16_t l[] = { sizeof(vendor), sizeof(ncm), sizeof(ecm) };
        fake_set_configs(FK_ECM, 0x0bda, 0x8156, 3, d, l);
        CHECK(usbh_get_hport_active_config_index(hp) == 2, "ECM preferred over NCM");
        fake_set_configs(FK_ECM, 0x0bda, 0x8156, 2, d, l);
        CHECK(usbh_get_hport_active_config_index(hp) == 1, "NCM when there is no ECM");
    }
    {
        const uint8_t *d[] = { vendor };
        const uint16_t l[] = { sizeof(vendor) };
        fake_set_configs(FK_ECM, 0xa69c, 0x8d80, 1, d, l);
        CHECK(usbh_get_hport_active_config_index(hp) == 0 && g_ctrl_count == 0,
              "single configuration: no requests (%u)", g_ctrl_count);
    }
    fake_hport(FK_ECM)->connected = false;
}

int main(int argc, char **argv)
{
    unsigned cycles = argc > 1 ? (unsigned)atoi(argv[1]) : 300;

    setvbuf(stdout, NULL, _IOLBF, 0);
    t_cfgsel();
    t_basic(FK_ECM, "cdc_ecm");
    t_basic(FK_RTL, "rtl8152");
    t_basic(FK_NCM, "cdc_ncm");
    t_basic(FK_ASIX, "asix");
    t_debounce();
    t_two();
    t_soak(cycles);
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
