/*
 * Host test of the station lifecycle in shim/wlan_dev_shim.c across USB resets: the RT-Thread
 * shim (rtt_shim.c, pthreads) and wlan_dev_shim.c are compiled unchanged; the WLAN offload
 * device, the libbsd ifnet and the USB host are fakes.
 *
 * Cases (each from a fresh start of the station):
 *  - first attach: wlan0 created, network found, connected
 *  - USB reset: detach removes wlan0 and every per-device state; the new instance comes back with
 *    wlan0 and rejoins, the first scan of the last channel only (board log of build 88: after a
 *    reset the station start failed once and wlan0 was never created again)
 *  - a station start that fails is retried with a backoff until it works
 *  - a warm-attached firmware that does not start twice is dropped and the stick re-enumerated
 *  - the single-channel scan that misses goes on with the full scan at once
 *
 * usage: run.sh (Linux; run-colima.sh on macOS)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <wlan_dev.h>

#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#include "aicwf_usbif.h"
#include "aicwf_wifi.h"
#include "aicwf_shim.h"

static int g_fail;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                         \
            g_fail++;                                                                     \
        }                                                                                 \
    } while (0)

/* ---- fakes: USB host, firmware, ifnet ---- */
static atomic_int f_reprobes, f_drop_warm, f_warm, f_ifnets, f_ifnet_attaches, f_link_up;
static char f_ifname[16];

unsigned aicwf_usbh_attach_count(void) { return 1; }
void aicwf_usbh_dump(void) {}
int aicwf_usbh_reprobe(void) { atomic_fetch_add(&f_reprobes, 1); return 0; }
void aicwf_usbh_start(void) {}
int aicwf_usbh_first_id(rt_uint16_t *vid, rt_uint16_t *pid) { *vid = 0xa69c; *pid = 0x8d81; return 1; }
void aicwf_fw_dump(void) {}
int aicwf_fw_install(void) { return 0; }
int aicwf_usb_rescan(int power_cycle) { (void)power_cycle; return 0; }
int aic8800_firmware_warm_attach(void) { return atomic_load(&f_warm); }
void aic8800_firmware_drop_warm(void) { atomic_fetch_add(&f_drop_warm, 1); atomic_store(&f_warm, 0); }

struct aicwf_ifnet { int dummy; };
static struct aicwf_ifnet f_ifn;

struct aicwf_ifnet *aicwf_ifnet_attach(const char *name, const uint8_t mac[6], aicwf_ifnet_tx_t tx, void *arg)
{
    (void)mac; (void)tx; (void)arg;
    CHECK(atomic_load(&f_ifnets) == 0); /* never two wlan0 */
    snprintf(f_ifname, sizeof(f_ifname), "%s", name);
    atomic_fetch_add(&f_ifnets, 1);
    atomic_fetch_add(&f_ifnet_attaches, 1);
    return &f_ifn;
}
int aicwf_ifnet_input(struct aicwf_ifnet *ifn, const void *frame, uint32_t len) { (void)ifn; (void)frame; (void)len; return 0; }
void aicwf_ifnet_link(struct aicwf_ifnet *ifn, int up) { (void)ifn; atomic_store(&f_link_up, up); }
void aicwf_ifnet_detach(struct aicwf_ifnet *ifn) { (void)ifn; atomic_store(&f_link_up, 0); atomic_fetch_sub(&f_ifnets, 1); }

/* ---- fake WLAN offload device ---- */
static struct rt_wlan_device f_wlan;
static atomic_int f_init_fail;      /* wlan_init failures still to return */
static atomic_int f_init_calls, f_scans, f_fast_scans, f_joins;
static atomic_int f_hide_on_channel; /* the AP moved: absent from a single-channel scan */
static atomic_int f_init_delay_ms;   /* a slow station start */
#define F_CHANNEL 6

static rt_err_t f_init(struct rt_wlan_device *w) {
    (void)w;
    atomic_fetch_add(&f_init_calls, 1);
    if (atomic_load(&f_init_delay_ms)) {
        usleep(atomic_load(&f_init_delay_ms) * 1000);
    }
    if (atomic_load(&f_init_fail) > 0) {
        atomic_fetch_sub(&f_init_fail, 1);
        return -RT_EBUSY;
    }
    return RT_EOK;
}
static rt_err_t f_mode(struct rt_wlan_device *w, rt_wlan_mode_t m) { (void)w; (void)m; return RT_EOK; }
static rt_err_t f_get_mac(struct rt_wlan_device *w, rt_uint8_t mac[]) {
    static const rt_uint8_t m[6] = {0x88, 0x00, 0x33, 0x77, 0x10, 0x01};
    (void)w;
    memcpy(mac, m, 6);
    return RT_EOK;
}
static rt_err_t f_scan(struct rt_wlan_device *w, struct rt_scan_info *si) {
    struct rt_wlan_info info;
    struct rt_wlan_buff b;
    int single = si->channel_min > 0 && si->channel_min == si->channel_max;

    atomic_fetch_add(&f_scans, 1);
    if (single) {
        atomic_fetch_add(&f_fast_scans, 1);
    }
    memset(&info, 0, sizeof(info));
    info.security = SECURITY_WPA2_AES_PSK;
    info.band = RT_802_11_BAND_2_4GHZ;
    info.channel = single && atomic_load(&f_hide_on_channel) ? 11 : F_CHANNEL;
    info.rssi = -50;
    info.ssid.len = 5;
    memcpy(info.ssid.val, "Chris", 5);
    memcpy(info.bssid, "\x02\x11\x22\x33\x44\x55", 6);
    if (!single || info.channel == si->channel_min) {
        b.data = &info;
        b.len = sizeof(info);
        rt_wlan_dev_indicate_event_handle(w, RT_WLAN_DEV_EVT_SCAN_REPORT, &b);
    }
    rt_wlan_dev_indicate_event_handle(w, RT_WLAN_DEV_EVT_SCAN_DONE, RT_NULL);
    return RT_EOK;
}
static rt_err_t f_join(struct rt_wlan_device *w, struct rt_sta_info *sta) {
    CHECK(sta->channel == F_CHANNEL || sta->channel == 11);
    atomic_fetch_add(&f_joins, 1);
    rt_wlan_dev_indicate_event_handle(w, RT_WLAN_DEV_EVT_CONNECT, RT_NULL);
    return RT_EOK;
}
static rt_err_t f_disconnect(struct rt_wlan_device *w) { (void)w; return RT_EOK; }
static int f_send(struct rt_wlan_device *w, void *b, int l) { (void)w; (void)b; (void)l; return RT_EOK; }

static const struct rt_wlan_dev_ops f_ops = {
    .wlan_init = f_init, .wlan_mode = f_mode, .wlan_scan = f_scan, .wlan_join = f_join,
    .wlan_disconnect = f_disconnect, .wlan_get_mac = f_get_mac, .wlan_send = f_send,
};

/* the driver's attach: register (attach work), then the hub thread's attach_done */
static void dev_attach(void)
{
    memset(&f_wlan, 0, sizeof(f_wlan)); /* the context is zeroed and reused on re-enumeration */
    CHECK(rt_wlan_dev_register_auto(&f_wlan, "aic8800d80", RT_WLAN_STATION, RT_WLAN_TRANSPORT_USB, &f_ops,
                                    RT_NULL) == RT_EOK);
    aicwf_wlan_attach_done();
}

static void dev_detach(void)
{
    CHECK(rt_wlan_dev_unregister(&f_wlan) == RT_EOK);
}

static void *hub_attach(void *arg)
{
    (void)arg;
    dev_attach();
    return NULL;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int wait_connected(double max_s, double *took)
{
    double t0 = now_s();
    int up = aicwf_wifi_wait_connected((int)(max_s * 1000));
    *took = now_s() - t0;
    return up;
}

int main(void)
{
    struct aicwf_wifi_status st;
    double took;
    int scans0, fast0;

    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(aicwf_wifi_start() == 0);
    CHECK(aicwf_wifi_connect("Chris", "password1") == 0);

    printf("== first attach\n");
    dev_attach();
    CHECK(wait_connected(5, &took) == 1);
    CHECK(aicwf_wifi_status(&st) == 0 && st.device_present && st.station_ready && st.connected);
    CHECK(strcmp(f_ifname, "wlan0") == 0 && atomic_load(&f_ifnets) == 1);
    aicwf_ifnet_addr_set("wlan0");
    printf("  connected after %.2f s\n", took);

    printf("== USB reset: detach, attach, rejoin on the last channel\n");
    aicwf_timing_mark(AICWF_T_LOST);
    dev_detach();
    CHECK(aicwf_wifi_status(&st) == 0 && !st.device_present && !st.station_ready && !st.connected);
    CHECK(atomic_load(&f_ifnets) == 0 && atomic_load(&f_link_up) == 0);
    scans0 = atomic_load(&f_scans);
    fast0 = atomic_load(&f_fast_scans);
    aicwf_timing_mark(AICWF_T_PLUG);
    dev_attach();
    CHECK(wait_connected(5, &took) == 1);
    aicwf_ifnet_addr_set("wlan0");
    CHECK(atomic_load(&f_ifnets) == 1);
    CHECK(atomic_load(&f_scans) == scans0 + 1 && atomic_load(&f_fast_scans) == fast0 + 1);
    CHECK(took < 1.0);
    printf("  rejoined after %.2f s (%d scan, single channel)\n", took, atomic_load(&f_scans) - scans0);

    printf("== USB reset, the station start fails once (-7): retried, wlan0 comes back\n");
    dev_detach();
    atomic_store(&f_init_fail, 1);
    dev_attach();
    CHECK(aicwf_wifi_status(&st) == 0 && st.device_present && !st.station_ready);
    CHECK(wait_connected(5, &took) == 1);
    CHECK(aicwf_wifi_status(&st) == 0 && st.station_ready && st.connected);
    CHECK(took >= 0.8 && took < 3.0); /* WLAN_START_RETRY_MIN_MS */
    printf("  wlan0 after a failed start, connected after %.2f s\n", took);

    printf("== USB reset, the station start fails three times: backoff 1, 2, 4 s\n");
    dev_detach();
    atomic_store(&f_init_fail, 3);
    dev_attach();
    CHECK(wait_connected(12, &took) == 1);
    CHECK(took >= 6.0 && took < 9.0);
    CHECK(atomic_load(&f_reprobes) == 0);
    printf("  connected after %.2f s\n", took);

    printf("== warm firmware that does not start: dropped, stick re-enumerated\n");
    dev_detach();
    atomic_store(&f_warm, 1);
    atomic_store(&f_init_fail, 1000);
    dev_attach();
    for (int i = 0; i < 60 && atomic_load(&f_reprobes) == 0; i++) {
        usleep(100000);
    }
    CHECK(atomic_load(&f_reprobes) == 1 && atomic_load(&f_drop_warm) == 1);
    /* the re-enumeration: the old instance goes, a reloaded one comes */
    dev_detach();
    atomic_store(&f_init_fail, 0);
    dev_attach();
    CHECK(wait_connected(5, &took) == 1);
    printf("  reloaded and connected after %.2f s\n", took);

    printf("== the AP moved channel: single-channel scan misses, full scan at once\n");
    dev_detach();
    atomic_store(&f_hide_on_channel, 1);
    scans0 = atomic_load(&f_scans);
    dev_attach();
    CHECK(wait_connected(5, &took) == 1);
    CHECK(atomic_load(&f_scans) == scans0 + 2);
    CHECK(took < 1.0);
    printf("  connected after %.2f s (%d scans)\n", took, atomic_load(&f_scans) - scans0);
    atomic_store(&f_hide_on_channel, 0);

    printf("== the device detaches while its station starts: no wlan0 left behind\n");
    {
        pthread_t t;

        dev_detach();
        atomic_store(&f_init_delay_ms, 300);
        pthread_create(&t, NULL, hub_attach, NULL);
        usleep(100000);
        dev_detach(); /* waits for the start in progress */
        pthread_join(t, NULL);
        CHECK(atomic_load(&f_ifnets) == 0);
        CHECK(aicwf_wifi_status(&st) == 0 && !st.device_present && !st.station_ready);
        atomic_store(&f_init_delay_ms, 0);
        dev_attach();
        CHECK(wait_connected(5, &took) == 1);
        CHECK(atomic_load(&f_ifnets) == 1);
    }

    printf("== detach during association attempts, many cycles\n");
    for (int i = 0; i < 50; i++) {
        dev_detach();
        dev_attach();
        if (i % 5 == 4) {
            CHECK(wait_connected(5, &took) == 1);
        }
    }
    CHECK(atomic_load(&f_ifnets) == 1);

    printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail != 0;
}
