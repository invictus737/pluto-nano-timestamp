/*
 * Independent QA tests of the USB Ethernet glue (net/usbnet.c) on the same fakes as
 * test_usbnet.c. fake_usbnet.c is included (not linked) so the tests can reach the device
 * models and class objects directly: a bulk IN error on a device that stays attached, a
 * second adapter of the same class, rapid cable flaps.
 *
 * Expected on the reviewed glue (commit c471ea0): qa_ecm_rx_error and qa_same_class FAIL
 * (documented defects), the others pass.
 *
 * usage: qa_usbnet [test-name]   (default: all)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake_usbnet.c"

#include "usbnet.h"

static int g_fail;
#define QCHECK(c, ...)                                                   \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);        \
            printf(__VA_ARGS__);                                         \
            printf("\n");                                                \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

static void qsleep(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int qwait_link(const char *ifn, int want, unsigned ms)
{
    for (unsigned t = 0; t < ms; t++) {
        if (fake_if_link(ifn) == want) {
            return 0;
        }
        qsleep(1);
    }
    return -1;
}

static int qwait_drained(int k, unsigned ms)
{
    for (unsigned t = 0; t < ms; t++) {
        if (fake_rx_pending(k) == 0) {
            return 0;
        }
        qsleep(1);
    }
    return -1;
}

/* a bulk IN transfer error (XACTERR exhausted, babble, stall) while the device stays attached:
 * the driver's rx thread goes back to find_class, clears connect_status and waits for the link
 * on the interrupt endpoint again */
static void inject_bulkin_error(int k)
{
    struct fdev *d = &g_fdev[k];

    pthread_mutex_lock(&d->m);
    d->kill_gen++;
    pthread_cond_broadcast(&d->c);
    pthread_mutex_unlock(&d->m);
}

static void push_n(int k, unsigned n, uint32_t seq0)
{
    for (unsigned i = 0; i < n; i++) {
        while (fake_rx_push(k, 100 + i, seq0 + i) != 0) {
            qsleep(1);
        }
    }
}

/* rx must resume after a transient bulk IN error while the link stays up and reported up */
static void qa_rx_error(int k, const char *name)
{
    struct fake_if_stats a, b;
    struct fake_dev_stats d0, d1;

    printf("qa: %s rx after a bulk IN error, device attached, link up\n", name);
    fake_plug(k);
    fake_cable(k, true);
    QCHECK(qwait_link("ue0", 1, 1500) == 0, "link up not reported");
    push_n(k, 10, 0);
    QCHECK(qwait_drained(k, 1000) == 0, "rx before the error");

    fake_if_stats(&a);
    inject_bulkin_error(k);
    qsleep(50);
    push_n(k, 10, 100);
    QCHECK(qwait_drained(k, 1500) == 0,
           "%s: rx stalled after one bulk IN error (%u frames pending, interface still link %d): "
           "the rx thread waits for a link notification the device never resends",
           name, fake_rx_pending(k), fake_if_link("ue0"));
    fake_dev_stats(k, &d0);
    for (uint32_t i = 0; i < 20; i++) {
        (void)fake_if_transmit("ue0", 200, i);
    }
    qsleep(200);
    fake_dev_stats(k, &d1);
    fake_if_stats(&b);
    QCHECK(d1.tx_refused == d0.tx_refused,
           "%s: %u transmits refused (connect_status cleared by the rx resync) while ue0 is link %d",
           name, d1.tx_refused - d0.tx_refused, fake_if_link("ue0"));
    QCHECK(b.link_downs == a.link_downs, "%s: spurious link down reported", name);

    fake_unplug(k);
    QCHECK(fake_if_count() == 0, "interfaces after unplug %d", fake_if_count());
    QCHECK(fake_wait_rx_gone(k, 1500) == 0, "rx thread did not end");
}

/*
 * A second adapter of the same class (two RTL8153 in ECM mode behind a hub, an ECM phone plus
 * an ECM dongle). CherryUSB keeps one static class object per driver: the second device's
 * connect() clears and refills it before usbh_X_run() (the glue's "already attached" check
 * comes too late), and both devices' interface priv point at it, so the second device's
 * unplug stops the first one's interface.
 */
static void qa_same_class(void)
{
    printf("qa: two adapters of one class (cdc_ecm)\n");
    fake_plug(FK_ECM);
    fake_cable(FK_ECM, true);
    QCHECK(qwait_link("ue0", 1, 1500) == 0, "first adapter link up");

    cdc_ecm_connect(); /* device B: upstream connect() memsets g_cdc_ecm_class, then run() */
    QCHECK(fake_if_count() == 2, "second ECM adapter got no interface (%d interfaces)", fake_if_count());

    cdc_ecm_disconnect(); /* device B unplugged: priv == &g_cdc_ecm_class -> stop(&g_cdc_ecm_class) */
    QCHECK(fake_if_link("ue0") != -2, "unplugging the second adapter detached the first adapter's ue0");

    /* clean up whatever is left */
    if (fake_if_link("ue0") != -2) {
        fake_unplug(FK_ECM);
    } else {
        struct fdev *d = &g_fdev[FK_ECM];
        pthread_mutex_lock(&d->m);
        d->hp.connected = false;
        d->kill_gen++;
        pthread_cond_broadcast(&d->c);
        pthread_mutex_unlock(&d->m);
    }
    QCHECK(fake_wait_rx_gone(FK_ECM, 1500) == 0, "rx thread left");
    QCHECK(fake_if_count() == 0, "interfaces left %d", fake_if_count());
}

/* rapid cable flaps: the reported state converges to the final cable state */
static void qa_flap(int k, const char *name)
{
    printf("qa: %s rapid link flap\n", name);
    fake_plug(k);
    for (int i = 0; i < 400; i++) {
        fake_cable(k, (i & 1) == 0);
        if ((i % 50) == 0) {
            qsleep(1);
        }
    }
    fake_cable(k, true);
    QCHECK(qwait_link("ue0", 1, 2500) == 0, "%s: final link up not reported (%d)", name, fake_if_link("ue0"));
    push_n(k, 10, 500);
    QCHECK(qwait_drained(k, 1500) == 0, "%s: rx after flaps", name);
    for (int i = 0; i < 101; i++) {
        fake_cable(k, (i & 1) != 0);
    }
    QCHECK(qwait_link("ue0", 0, 2500) == 0, "%s: final link down not reported (%d)", name, fake_if_link("ue0"));
    fake_unplug(k);
    QCHECK(fake_wait_rx_gone(k, 1500) == 0, "rx thread left");
    QCHECK(fake_if_count() == 0, "interfaces left %d", fake_if_count());
}

/* unplug while the rx thread is inside nx_input and the tx taskqueue inside eth_output,
 * all four classes at once on ue0..ue3, replugged in a different order */
static volatile int g_q_run;
static void *q_traffic(void *a)
{
    uint32_t seq = 0;
    (void)a;
    while (g_q_run) {
        for (int k = 0; k < FK_N; k++) {
            char n[8];
            snprintf(n, sizeof(n), "ue%d", k);
            (void)fake_if_transmit(n, 60 + (seq % 1400), seq);
            (void)fake_rx_push(k, 60 + (seq % 1400), seq);
        }
        seq++;
    }
    return NULL;
}

static void qa_four_flood(unsigned cycles)
{
    pthread_t t;

    printf("qa: four adapters, unplug under rx/tx flood, %u cycles\n", cycles);
    g_q_run = 1;
    pthread_create(&t, NULL, q_traffic, NULL);
    for (unsigned c = 0; c < cycles; c++) {
        for (int i = 0; i < FK_N; i++) {
            int k = (int)((i + c) % FK_N);
            fake_plug(k);
            fake_cable(k, true);
        }
        qsleep(c % 7);
        for (int i = 0; i < FK_N; i++) {
            fake_unplug((int)((i * 3 + c) % FK_N));
        }
    }
    g_q_run = 0;
    pthread_join(t, NULL);
    for (int k = 0; k < FK_N; k++) {
        struct fake_dev_stats d;
        QCHECK(fake_wait_rx_gone(k, 2000) == 0, "device %d: rx threads left", k);
        fake_dev_stats(k, &d);
        QCHECK(d.rx_threads_max <= 1, "device %d: %u rx threads at once", k, d.rx_threads_max);
    }
    QCHECK(fake_if_count() == 0, "interfaces left %d", fake_if_count());
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;

    setvbuf(stdout, NULL, _IOLBF, 0);
#define RUN(name, call)                                  \
    if (only == NULL || strcmp(only, name) == 0) {       \
        call;                                            \
    }
    RUN("qa_rtl_rx_error", qa_rx_error(FK_RTL, "rtl8152"));
    RUN("qa_asix_rx_error", qa_rx_error(FK_ASIX, "asix"));
    RUN("qa_flap_ecm", qa_flap(FK_ECM, "cdc_ecm"));
    RUN("qa_flap_rtl", qa_flap(FK_RTL, "rtl8152"));
    RUN("qa_four_flood", qa_four_flood(150));
    RUN("qa_ecm_rx_error", qa_rx_error(FK_ECM, "cdc_ecm"));
    RUN("qa_ncm_rx_error", qa_rx_error(FK_NCM, "cdc_ncm"));
    RUN("qa_same_class", qa_same_class());
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
