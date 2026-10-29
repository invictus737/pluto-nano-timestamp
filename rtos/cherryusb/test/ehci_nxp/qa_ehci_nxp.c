/*
 * QA tests of the ehci_nxp driver, on top of the base harness (test_ehci_nxp.c is included
 * unchanged, its main renamed). Built by qa_run.sh with a counting register hook (qa_regs.h)
 * and the critical section wrapped (-Wl,--wrap) to measure its hold time in simulated time.
 *
 *   qa_idle_60s        armed Wi-Fi/BT receive URBs, NAKing device, 60 s: interrupts, register
 *                      accesses, schedule stops, glue wakeups (1 s watchdog model)
 *   qa_steady_cost     BT events + ACL + Wi-Fi: per-completion register accesses, barriers,
 *                      critical-section hold, no schedule stop/toggle rewrite in steady state
 *   qa_int_interval    HS interrupt bInterval 1..16: frame list placement, S-mask, measured poll rate
 *   qa_pages_zlp       bulk OUT/IN 1..60000 bytes at any 32-byte offset (page crossing, several
 *                      qTDs, short packet via alt-next), 512-multiple OUT followed by a ZLP URB
 *   qa_deferred_kill   the AIC adapter's kill loop (aicwf_cherry_kill_urb: usbh_kill_urb() under
 *                      the critical section until the request leaves REQ_HW) when the schedule
 *                      handshake is missed
 *   qa_error_storm     XACTERR/NAK/STALL storm on every pipe, then recovery without reset
 *   qa_hc_recover      host system error and a wedged core (AS/PS never follow): controller reset,
 *                      URBs back, gate until the port reset, re-enumeration, traffic; reset backoff
 *   qa_soak            replug + re-enumeration WITHOUT controller reset, random NAK/latency/
 *                      errors/AS lag/kills, until QA_SOAK completions (default 300000)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define main ehci_nxp_base_main
#include "test_ehci_nxp.c"
#undef main

uint64_t g_qa_reg_rd, g_qa_reg_wr;

/* ---- critical section hold time (simulated ns; register accesses and spins advance time) ---- */
size_t __real_usb_osal_enter_critical_section(void);
void __real_usb_osal_leave_critical_section(size_t flag);
static uint64_t g_crit_t0, g_crit_max_ns, g_crit_total_ns, g_crit_enters;

size_t __wrap_usb_osal_enter_critical_section(void)
{
    if (g_sim_crit == 0) {
        g_crit_t0 = sim_now_ns();
        g_crit_enters++;
    }
    return __real_usb_osal_enter_critical_section();
}

void __wrap_usb_osal_leave_critical_section(size_t flag)
{
    if (g_sim_crit == 1) {
        uint64_t d = sim_now_ns() - g_crit_t0;
        g_crit_total_ns += d;
        if (d > g_crit_max_ns) {
            g_crit_max_ns = d;
        }
    }
    __real_usb_osal_leave_critical_section(flag);
}

struct qa_snap {
    uint64_t rd, wr, irq, barriers, crit_enters, uframes;
    struct ehci_nxp_stats st;
};

static struct qa_snap qa_snap(void)
{
    struct qa_snap s = { g_qa_reg_rd, g_qa_reg_wr, g_sim_stats.irq_calls, g_sim_barriers, g_crit_enters,
                         g_sim_stats.uframes, H()->stats };
    return s;
}

/* the glue's poll (usb_glue_zynq.c): every 1 s, every 2 ms while work is pending */
static uint32_t g_glue_wakeups, g_glue_fast_wakeups;
static void glue_poll(void)
{
    size_t f = usb_osal_enter_critical_section();
    g_glue_wakeups++;
    if (usbh_ehci_iaa_pending(0)) {
        g_glue_fast_wakeups++;
    }
    (void)usbh_ehci_async_watchdog(0);
    usb_osal_leave_critical_section(f);
}

/* pump with the glue poll modelled (1 s idle period, 2 ms while pending) */
static void pump_glue_us(uint64_t us)
{
    static uint64_t next_poll;
    uint64_t end = sim_now_ns() + us * 1000ULL;
    while (sim_now_ns() < end) {
        pump_us(125);
        if (next_poll == 0 || sim_now_ns() >= next_poll) {
            glue_poll();
            next_poll = sim_now_ns() + (usbh_ehci_iaa_pending(0) ? 2000000ULL : 1000000000ULL);
        } else if (usbh_ehci_iaa_pending(0) && next_poll > sim_now_ns() + 2000000ULL) {
            next_poll = sim_now_ns() + 2000000ULL;
        }
    }
}

static void arm_receivers(void)
{
    stream_init(&s_in, 0x81, 0, 0);
    stream_init(&s_min, 0x82, 0, 0);
    stream_init(&s_evt, 0x83, 0, 0);
    stream_init(&s_ain, 0x84, 0, 0);
    for (int i = 0; i < 4; i++) {
        wk_init(&w_in[i], &g_ep_in_data, &s_in, 4096);
        w_in[i].want = 1;
        wk_register(&w_in[i]);
    }
    for (int i = 0; i < 2; i++) {
        wk_init(&w_min[i], &g_ep_in_msg, &s_min, 1024);
        w_min[i].want = 1;
        wk_register(&w_min[i]);
        wk_init(&w_ain[i], &g_ep_acl_in, &s_ain, 4096);
        w_ain[i].want = 1;
        wk_register(&w_ain[i]);
    }
    wk_init(&w_evt, &g_ep_evt, &s_evt, 4096);
    w_evt.rx = rx_events;
    w_evt.want = 1;
    wk_register(&w_evt);
}

/* ================= idle ================= */
static void t_qa_idle_60s(void)
{
    struct qa_snap a, b;

    bringup();
    enumerate(30);
    g_cc_expected = g_cc_got = g_ncp_pkts = g_evt_bad = 0;
    g_cc_head = g_cc_tail = 0;
    arm_receivers();
    pump_us(20000);
    CHECK(busy_workers() == 9);
    g_glue_wakeups = g_glue_fast_wakeups = 0;
    a = qa_snap();
    g_crit_max_ns = 0;
    pump_glue_us(60ULL * 1000000ULL);
    b = qa_snap();
    printf("  idle 60 s, 9 receive URBs armed (4 Wi-Fi data, 2 msg, BT event, 2 ACL), device NAKs:\n"
           "    interrupts %lu, driver register reads %lu writes %lu, barriers %lu, critical sections %lu,\n"
           "    async stops %u periodic stops %u toggle rewrites %u handshake sleeps %u, glue wakeups %u (fast %u),\n"
           "    HC NAKed polls %lu (no CPU), max critical section %.1f us\n",
           (unsigned long)(b.irq - a.irq), (unsigned long)(b.rd - a.rd), (unsigned long)(b.wr - a.wr),
           (unsigned long)(b.barriers - a.barriers), (unsigned long)(b.crit_enters - a.crit_enters),
           b.st.async_stops - a.st.async_stops, b.st.periodic_stops - a.st.periodic_stops,
           b.st.toggle_sets - a.st.toggle_sets, b.st.handshake_sleeps - a.st.handshake_sleeps, g_glue_wakeups,
           g_glue_fast_wakeups, (unsigned long)g_sim_stats.naks, g_crit_max_ns / 1e3);
    CHECKV(b.irq == a.irq, "%lu interrupts while idle", (unsigned long)(b.irq - a.irq));
    CHECKV(b.rd + b.wr == a.rd + a.wr, "%lu driver register accesses while idle",
           (unsigned long)(b.rd + b.wr - a.rd - a.wr));
    CHECK(b.st.async_stops == a.st.async_stops && b.st.periodic_stops == a.st.periodic_stops);
    CHECKV(g_glue_wakeups <= 61 && g_glue_fast_wakeups == 0, "glue wakeups %u fast %u", g_glue_wakeups,
           g_glue_fast_wakeups);
    CHECK(busy_workers() == 9);
    check_accounting("idle");
    check_no_violations();
    close_all_and_check();
}

/* ================= steady-state cost ================= */
static void t_qa_steady_cost(void)
{
    struct qa_snap a, b;
    uint64_t compl;
    uint32_t acl_sent = 0;

    bringup();
    enumerate(31);
    g_sim_cfg.stale_cache = 1;
    g_dev_cfg.nak_permille = 50;
    g_cc_expected = g_cc_got = g_ncp_pkts = g_evt_bad = 0;
    g_cc_head = g_cc_tail = 0;
    start_wifi(4, 2);
    sim_dev_in_enqueue(0x81, 1000000, 20, 1600);
    stream_init(&s_evt, 0x83, 0, 0);
    wk_init(&w_evt, &g_ep_evt, &s_evt, 4096);
    w_evt.rx = rx_events;
    w_evt.want = 1;
    wk_register(&w_evt);
    stream_init(&s_ain, 0x84, 0, 0);
    wk_init(&w_ain[0], &g_ep_acl_in, &s_ain, 4096);
    w_ain[0].want = 1;
    wk_register(&w_ain[0]);
    stream_init(&s_aout, 0x03, 300, 1000);
    wk_init(&w_aout, &g_ep_acl_out, &s_aout, 1024);
    wk_register(&w_aout);
    wk_submit(&w_aout);
    acl_sent++;
    pump_us(20000);
    a = qa_snap();
    g_crit_max_ns = 0;
    g_crit_total_ns = 0;
    for (int ms = 0; ms < 5000; ms++) {
        if (ms % 25 == 3) {
            CHECK(hci_cmd((uint16_t)(0x0C00 + (ms & 0xff))) == 4);
        }
        if (ms % 10 == 0 && !w_aout.busy) {
            wk_submit(&w_aout);
            acl_sent++;
        }
        if (ms % 20 == 0) {
            sim_dev_in_enqueue(0x84, 1, 30, 1000);
        }
        pump_us(1000);
    }
    b = qa_snap();
    compl = b.st.completions - a.st.completions;
    printf("  steady 5 s Wi-Fi + BT: %lu completions, %lu interrupts (%.2f compl/irq)\n"
           "    per completion: %.2f register reads %.2f writes, %.2f DMA barriers, %.2f critical sections\n"
           "    critical section max %.1f us, total %.1f ms (%.2f %% of time, simulated MMIO cost)\n"
           "    async stops %u periodic stops %u toggle rewrites %u handshake sleeps %u xacterr %u\n",
           (unsigned long)compl, (unsigned long)(b.irq - a.irq), (double)compl / (double)(b.irq - a.irq + 1),
           (double)(b.rd - a.rd) / compl, (double)(b.wr - a.wr) / compl, (double)(b.barriers - a.barriers) / compl,
           (double)(b.crit_enters - a.crit_enters) / compl, g_crit_max_ns / 1e3, g_crit_total_ns / 1e6,
           g_crit_total_ns / 1e7 / 5.0 * 1.0, b.st.async_stops - a.st.async_stops,
           b.st.periodic_stops - a.st.periodic_stops, b.st.toggle_sets - a.st.toggle_sets,
           b.st.handshake_sleeps - a.st.handshake_sleeps, b.st.xacterr_restarts - a.st.xacterr_restarts);
    CHECK(b.st.async_stops == a.st.async_stops);
    CHECK(b.st.periodic_stops == a.st.periodic_stops);
    CHECK(b.st.toggle_sets == a.st.toggle_sets);
    CHECKV(g_cc_got == g_cc_expected, "Command Complete %u of %u", g_cc_got, g_cc_expected);
    CHECK(g_evt_bad == 0);
    check_wifi_integrity();
    check_accounting("steady");
    check_no_violations();
    close_all_and_check();
}

/* ================= interrupt interval ================= */
static void t_qa_int_interval(void)
{
    for (uint8_t bi = 1; bi <= 16; bi++) {
        struct usb_endpoint_descriptor e;
        struct ehci_nxp_pipe *p;
        uint32_t uf = 1U << (bi - 1), eff = uf > 8192 ? 8192 : uf, nent = 0, bad = 0, sm;
        uint64_t polls0, polls;

        bringup();
        enumerate(40);
        mk_ep(&e, 0x83, USB_ENDPOINT_TYPE_INTERRUPT, 16, bi);
        stream_init(&s_evt, 0x83, 0, 0);
        wk_init(&w_evt, &e, &s_evt, 64);
        w_evt.rx = NULL;
        w_evt.want = 1;
        wk_register(&w_evt);
        wk_submit(&w_evt);
        pump_us(3000);
        p = pipe_of(0x83, USB_ENDPOINT_TYPE_INTERRUPT);
        CHECK(p != NULL);
        if (!p) {
            close_all_and_check();
            continue;
        }
        for (uint32_t f = 0; f < CONFIG_USB_EHCI_FRAME_LIST_SIZE; f++) {
            int has = (H()->framelist[f] & ~0x1FU) == ((uint32_t)(uintptr_t)&p->qh->hw & ~0x1FU);
            int want = (f % (eff >= 8 ? eff / 8 : 1)) == 0;
            nent += has;
            bad += has != want;
        }
        sm = p->qh->hw.epcap & 0xff;
        polls0 = g_dev_stats.in[3].naks + g_dev_stats.in[3].packets;
        pump_us(2048000);
        polls = g_dev_stats.in[3].naks + g_dev_stats.in[3].packets - polls0;
        {
            uint64_t expect = 16384 / eff;
            int ok = polls + 1 >= expect && polls <= expect + 1;
            printf("  bInterval %2u: %5u uframes, frame list entries %4u, S-mask %02x, polls in 2.048 s %lu "
                   "(expected %lu)%s\n",
                   bi, uf, nent, sm, (unsigned long)polls, (unsigned long)expect,
                   uf > 8192 ? " [capped at the 1024-frame list]" : "");
            CHECKV(bad == 0, "bInterval %u: %u frame list entries misplaced", bi, bad);
            CHECKV(ok, "bInterval %u: %lu polls, expected %lu", bi, (unsigned long)polls, (unsigned long)expect);
        }
        check_no_violations();
        close_all_and_check();
    }
}

/* ================= page crossing and ZLP ================= */
static struct usbh_urb g_qu;
static volatile int g_qu_done, g_qu_res;
static void qu_done(void *arg, int n)
{
    (void)arg;
    g_qu_done = 1;
    g_qu_res = n;
}

static int qu_run(struct usb_endpoint_descriptor *ep, uint8_t *buf, uint32_t len, uint8_t *toggle)
{
    memset(&g_qu, 0, sizeof(g_qu));
    usbh_bulk_urb_fill(&g_qu, &g_hp, ep, buf, len, 0, qu_done, NULL);
    g_qu.data_toggle = *toggle;
    g_qu_done = 0;
    if (usbh_submit_urb(&g_qu) != 0) {
        return -1000;
    }
    for (int i = 0; i < 2000 && !g_qu_done; i++) {
        pump_us(250);
    }
    if (!g_qu_done) {
        size_t f = usb_osal_enter_critical_section();
        usbh_kill_urb(&g_qu);
        usb_osal_leave_critical_section(f);
        return -2000;
    }
    *toggle = g_qu.data_toggle;
    return g_qu_res;
}

static void t_qa_pages_zlp(void)
{
    const uint32_t sz = 3 * 65536;
    uint8_t *ob, *ib, tog_o = 0, tog_i = 0;
    uint32_t sent = 0, zlps = 0, got = 0, rx_bad = 0, multi = 0;

    bringup();
    enumerate(41);
    g_dev_cfg.nak_permille = 100;
    ob = sim_dma_alloc(sz, 4096);
    ib = sim_dma_alloc(sz, 4096);
    sim_cache_track(ob, sz, 0);
    sim_cache_track(ib, sz, 1);
    for (int i = 0; i < 400; i++) {
        uint32_t off = (sim_rand() % 128) * 32, len;
        int rc;

        switch (i % 4) {
        case 0: len = 512 * (1 + sim_rand() % 32); break;      /* 512-multiple: ZLP needed */
        case 1: len = 16384; break;                            /* one full qTD, 5 pages if unaligned */
        case 2: len = 8 + sim_rand() % 16376; break;
        default: len = 4096 - off + sim_rand() % 64; break;    /* ends right after a page */
        }
        /* the device model keeps 16 KiB per OUT frame (sim_dev.c g_outbuf): multi-qTD OUT is
           covered by the IN side (same qTD builder) and the base suite's 2 KiB frames */
        len = sim_frame_wifi(ob + off, len, sent, 0x01);
        sim_cache_cpu_wrote(ob + off, len);
        multi += len > 16384;
        rc = qu_run(&g_ep_out_data, ob + off, len, &tog_o);
        CHECKV(rc == (int)len, "OUT %u bytes at offset %u: %d", len, off, rc);
        if (len % 512 == 0) {
            rc = qu_run(&g_ep_out_data, NULL, 0, &tog_o); /* the adapter's URB_ZERO_PACKET */
            CHECKV(rc == 0, "ZLP after %u bytes: %d", len, rc);
            zlps++;
        }
        sent++;
        /* IN: a frame of any size into a 61440-byte URB at any offset */
        sim_dev_in_enqueue(0x81, 1, 1, 60000);
        rc = qu_run(&g_ep_in_data, ib + off, 61440, &tog_i);
        if (rc > 0) {
            int64_t seq;
            sim_cache_invalidate((uintptr_t)(ib + off), (size_t)rc);
            seq = sim_frame_check(ib + off, (uint32_t)rc, 0x81);
            if (seq != (int64_t)got) {
                rx_bad++;
            }
            got++;
        } else {
            CHECKV(0, "IN at offset %u: %d", off, rc);
        }
    }
    printf("  page/ZLP: %u OUT frames (%u > 16 KiB, %u ZLPs), device got %lu (bad %lu order %lu); IN %u frames, "
           "%u bad\n",
           sent, multi, zlps, (unsigned long)g_dev_stats.out[1].frames, (unsigned long)g_dev_stats.out[1].bad,
           (unsigned long)g_dev_stats.out[1].dup_or_gap, got, rx_bad);
    CHECK(g_dev_stats.out[1].frames == sent && g_dev_stats.out[1].bad == 0 && g_dev_stats.out[1].dup_or_gap == 0);
    CHECK(rx_bad == 0 && got == 400);
    CHECK(g_dev_stats.toggle_errors == 0);
    CHECKV(g_viol[V_CPAGE] == 0, "%u buffer page overruns", g_viol[V_CPAGE]);
    check_no_violations();
    sim_cache_untrack(ob);
    sim_cache_untrack(ib);
    sim_dma_free(ob);
    sim_dma_free(ib);
    close_all_and_check();
}

/* ================= deferred cancel under the adapter's kill loop ================= */
static void t_qa_deferred_kill(void)
{
    uint32_t loops = 0;
    uint64_t t0;
    size_t f;

    bringup();
    enumerate(42);
    stream_init(&s_in, 0x81, 0, 0);
    wk_init(&w_in[0], &g_ep_in_data, &s_in, 4096);
    wk_register(&w_in[0]);
    wk_submit(&w_in[0]);
    pump_us(5000);
    CHECK(w_in[0].busy);
    g_sim_cfg.as_delay_uf = 1600; /* USBSTS.AS follows USBCMD.ASE after 200 ms (> 100 ms bound) */
    t0 = sim_now_ns();
    /* aicwf_cherry_kill_urb(): CRIT_ENTER; for (;;) { REQ_HW: usbh_kill_urb(); continue; } */
    f = usb_osal_enter_critical_section();
    while (w_in[0].busy && loops < 100000) {
        usbh_kill_urb(&w_in[0].urb);
        loops++;
    }
    usb_osal_leave_critical_section(f);
    printf("  deferred kill: %u usbh_kill_urb() calls under the critical section in %.1f ms simulated, "
           "URB %s, handshake misses %u, deferred %u\n",
           loops, (sim_now_ns() - t0) / 1e6, w_in[0].busy ? "STILL IN THE CONTROLLER (adapter loop spins forever)" :
                                                            "completed",
           H()->stats.handshake_timeouts, H()->stats.deferred);
    CHECKV(!w_in[0].busy, "usbh_kill_urb() returned 0 %u times without completing the URB: "
                          "aicwf_cherry_kill_urb() would spin with the critical section held, the "
                          "usbh_irq thread (ehci_nxp_service) can never run", loops);
    /* with the lock released, the glue's 2 ms poll finishes the cancel */
    pump_glue_us(400000);
    CHECKV(!w_in[0].busy, "deferred cancel never finished");
    g_sim_cfg.as_delay_uf = 1;
    check_accounting("deferred kill");
    sim_viol_clear(); /* the forced AS lag is not a driver violation */
    close_all_and_check();
}

/* ================= error storm and recovery ================= */
static void t_qa_error_storm(void)
{
    uint64_t f0;

    bringup();
    enumerate(43);
    g_sim_cfg.stale_cache = 1;
    g_cc_expected = g_cc_got = g_ncp_pkts = g_evt_bad = 0;
    g_cc_head = g_cc_tail = 0;
    start_wifi(4, 2);
    stream_init(&s_evt, 0x83, 0, 0);
    wk_init(&w_evt, &g_ep_evt, &s_evt, 4096);
    w_evt.rx = rx_events;
    w_evt.want = 1;
    wk_register(&w_evt);
    sim_dev_in_enqueue(0x81, 1000000, 20, 1600);
    g_dev_cfg.noresp_permille = 400;
    g_dev_cfg.nak_permille = 300;
    for (int ms = 0; ms < 2000; ms++) {
        if (ms % 150 == 0) {
            sim_dev_halt_ep(0x81);
        }
        if (ms % 150 == 75) {
            int rc = ctrl(0x02, 1, 0, 0x81, 0, 500);
            if (rc == 0) {
                s_in.toggle = 0;
            }
        }
        pump_us(1000);
    }
    g_dev_cfg.noresp_permille = 0;
    g_dev_cfg.nak_permille = 0;
    while (ctrl(0x02, 1, 0, 0x81, 0, 500) != 0) {
        pump_us(1000);
    }
    s_in.toggle = 0;
    for (int i = 0; i < 4; i++) {
        if (!w_in[i].busy) {
            wk_submit(&w_in[i]);
        }
    }
    pump_us(50000);
    f0 = s_in.frames;
    {
        uint64_t o0 = s_out.frames, d0 = g_dev_stats.out[1].frames, bad0 = g_dev_stats.out[1].bad, tg0 = g_dev_stats.toggle_errors;
        uint32_t cc0 = g_cc_expected;
        for (int ms = 0; ms < 1000; ms++) {
            if (ms % 50 == 0) {
                CHECK(hci_cmd((uint16_t)(0x3000 + ms)) == 4);
            }
            pump_us(1000);
        }
        printf("  storm: xacterr restarts %u stalls %u errors %u; after: IN %lu OUT %lu frames, HCI %u/%u, "
               "toggle errors during %lu after %lu\n",
               H()->stats.xacterr_restarts, H()->stats.stalls, H()->stats.errors,
               (unsigned long)(s_in.frames - f0), (unsigned long)(s_out.frames - o0), g_cc_got, g_cc_expected,
               (unsigned long)tg0, (unsigned long)(g_dev_stats.toggle_errors - tg0));
        CHECK(s_in.frames - f0 > 1000);
        CHECK(s_out.frames - o0 > 1000);
        CHECK(g_dev_stats.out[1].frames - d0 == s_out.frames - o0);
        CHECK(g_dev_stats.out[1].bad == bad0);
        CHECK(g_dev_stats.toggle_errors == tg0);
        CHECK(g_cc_expected > cc0);
    }
    check_accounting("error storm");
    check_no_violations();
    close_all_and_check();
}

/* ================= soak: replug/re-enumeration without controller reset ================= */
static void replug(uint8_t addr)
{
    struct pools p;
    uint32_t st;

    g_stop = 1;
    for (int i = 0; i < g_nwk; i++) {
        g_wk[i]->want = 0;
    }
    sim_port_disconnect();
    pump_us(3000);
    g_hp.connected = false;
    {
        size_t f = usb_osal_enter_critical_section();
        usbh_kill_urb(&g_ctl_urb);
        usb_osal_leave_critical_section(f);
    }
    pump_glue_us(5000);
    p = pools_now();
    CHECKV(p.pipes == 0 && p.xfers == 0 && p.qtds_free == CONFIG_USB_EHCI_QTD_NUM,
           "after unplug: pipes %u xfers %u qtds %u", p.pipes, p.xfers, p.qtds_free);
    CHECKV(busy_workers() == 0, "%d URBs never handed back", busy_workers());
    for (int i = 0; i < g_nwk; i++) {
        wk_free(g_wk[i]);
    }
    g_nwk = 0;
    memset(&g_dev_stats, 0, sizeof(g_dev_stats));
    sim_port_connect();
    sim_advance_ms(2);
    CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_CLEAR_FEATURE,
                 HUB_PORT_FEATURE_C_CONNECTION, 1, NULL) == 0);
    CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_SET_FEATURE, HUB_PORT_FEATURE_RESET, 1,
                 NULL) == 0);
    st = port_status();
    CHECK(st & (1U << HUB_PORT_FEATURE_ENABLE));
    hport_init();
    g_stop = 0;
    enumerate(addr);
}

static void t_qa_soak(void)
{
    const char *e = getenv("QA_SOAK");
    uint64_t target = e && *e ? strtoull(e, NULL, 0) : 300000, total = 0;
    uint32_t cycle = 0, fails0;
    uint64_t kills = 0, tmo = 0;

    bringup();
    enumerate(50);
    while (total < target && g_fail < 10) {
        uint32_t ms_run = 300 + sim_rand() % 1200;
        uint64_t c0 = H()->stats.completions;
        uint64_t in_kills = 0;
        char cfgdesc[96];

        fails0 = g_fail;
        g_sim_cfg.stale_cache = sim_rand() & 1;
        g_sim_cfg.irq_latency_uf = sim_rand() % 4;
        g_sim_cfg.as_lag_max_uf = (sim_rand() % 3 == 0) ? sim_rand() % 40 : 0;
        g_sim_cfg.preempt = sim_rand() & 1;
        g_dev_cfg.nak_permille = sim_rand() % 500;
        g_dev_cfg.noresp_permille = sim_rand() % 8;
        g_dev_cfg.msg_latency_uf = sim_rand() % 128;
        g_cc_expected = g_cc_got = g_ncp_pkts = g_evt_bad = 0;
        g_cc_head = g_cc_tail = 0;
        start_wifi(1 + sim_rand() % 4, 1 + sim_rand() % 2);
        for (int i = 0; i < 4; i++) {
            w_in[i].timeout_ms = (sim_rand() % 3 == 0) ? 2 + sim_rand() % 30 : 0;
        }
        stream_init(&s_evt, 0x83, 0, 0);
        wk_init(&w_evt, &g_ep_evt, &s_evt, 4096);
        w_evt.rx = rx_events;
        w_evt.want = 1;
        wk_register(&w_evt);
        stream_init(&s_ain, 0x84, 0, 0);
        wk_init(&w_ain[0], &g_ep_acl_in, &s_ain, 4096);
        w_ain[0].want = 1;
        wk_register(&w_ain[0]);
        for (uint32_t ms = 0; ms < ms_run; ms++) {
            if (sim_rand() % 4 == 0) {
                sim_dev_in_enqueue(0x81, 1 + sim_rand() % 6, 1, 4000);
            }
            if (ms % 40 == 0) {
                sim_dev_in_enqueue(0x84, 1, 10, 900);
            }
            if (ms % 61 == 0) {
                int n = hci_cmd((uint16_t)(0x2000 + (ms & 0xff)));
                CHECKV(n == 4, "cycle %u HCI command %d", cycle, n);
            }
            pump_glue_us(1000);
        }
        /* quiesce: no new URBs, no errors, no timeouts, drain */
        snprintf(cfgdesc, sizeof(cfgdesc), "nak %u noresp %u lag %u preempt %d stale %d irqlat %u",
                 g_dev_cfg.nak_permille, g_dev_cfg.noresp_permille, g_sim_cfg.as_lag_max_uf, g_sim_cfg.preempt,
                 g_sim_cfg.stale_cache, g_sim_cfg.irq_latency_uf);
        for (int i = 0; i < g_nwk; i++) {
            g_wk[i]->want = 0;
        }
        g_dev_cfg.nak_permille = 0;
        g_dev_cfg.noresp_permille = 0;
        for (int i = 0; i < 4; i++) {
            in_kills += w_in[i].tmo;
            w_in[i].timeout_ms = 0;
        }
        pump_glue_us(60000);
        kills += in_kills;
        tmo += in_kills;
        CHECKV(s_in.bad <= in_kills, "cycle %u: %lu corrupt IN frames, %lu cancels", cycle, (unsigned long)s_in.bad,
               (unsigned long)in_kills);
        CHECKV(g_dev_stats.out[1].bad == 0 && g_dev_stats.out[1].dup_or_gap == 0, "cycle %u: OUT %lu bad %lu order",
               cycle, (unsigned long)g_dev_stats.out[1].bad, (unsigned long)g_dev_stats.out[1].dup_or_gap);
        CHECKV(g_dev_stats.out[1].frames == s_out.frames, "cycle %u: device %lu OUT frames, host %lu", cycle,
               (unsigned long)g_dev_stats.out[1].frames, (unsigned long)s_out.frames);
        CHECKV(g_dev_stats.toggle_errors == 0, "cycle %u: %lu toggle errors", cycle,
               (unsigned long)g_dev_stats.toggle_errors);
        CHECKV(g_cc_got == g_cc_expected && g_evt_bad == 0, "cycle %u: HCI %u/%u bad %u", cycle, g_cc_got,
               g_cc_expected, g_evt_bad);
        CHECKV(s_ain.bad == 0 && s_ain.gap == 0, "cycle %u: ACL IN bad %lu gap %lu", cycle, (unsigned long)s_ain.bad,
               (unsigned long)s_ain.gap);
        check_accounting("soak");
        if (sim_viol_total()) {
            printf("  cycle %u: violations\n", cycle);
            check_no_violations();
            sim_viol_clear();
        }
        total += H()->stats.completions - c0;
        cycle++;
        if (g_fail != fails0) {
            printf("  cycle %u failed (%s)\n", cycle - 1, cfgdesc);
        }
        replug((uint8_t)(1 + cycle % 120));
    }
    printf("  soak: %u replug cycles without controller reset, %lu completions, %lu timeout kills, "
           "kills %u, handshake misses %u, deferred %u, xacterr %u, max handshake %u us, t=%.1f s simulated\n",
           cycle, (unsigned long)total, (unsigned long)tmo, H()->stats.kills, H()->stats.handshake_timeouts,
           H()->stats.deferred, H()->stats.xacterr_restarts, H()->stats.max_handshake_us, sim_now_ns() / 1e9);
    CHECK(total >= target);
    check_no_violations();
    close_all_and_check();
}


/* ================= controller death: HSE and a wedged core, reset and re-enumeration ================= */
/* what the hub thread does after a controller reset: connection change seen, port reset, enumerate */
static void rehub_after_reset(uint8_t addr)
{
    uint32_t st = port_status();
    struct pools p = pools_now();

    CHECKV(st & (1U << HUB_PORT_FEATURE_C_CONNECTION), "no connection change after the reset (0x%x)", st);
    CHECKV(B->hcd.roothub.int_buffer[0] & 2, "hub thread not woken");
    CHECKV(p.pipes == 0 && p.xfers == 0 && p.qtds_free == CONFIG_USB_EHCI_QTD_NUM,
           "after the reset: pipes %u xfers %u qtds %u", p.pipes, p.xfers, p.qtds_free);
    CHECKV(busy_workers() == 0, "%d URBs not handed back by the reset", busy_workers());
    g_stop = 1;
    for (int i = 0; i < g_nwk; i++) {
        g_wk[i]->want = 0;
    }
    /* gated until the port reset: an URB of the old device is refused */
    CHECK(ctrl(0x80, 6, 0x0100, 0, 8, 100) == -USB_ERR_NOTCONN);
    B->hcd.roothub.int_buffer[0] = 0;
    CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_CLEAR_FEATURE,
                 HUB_PORT_FEATURE_C_CONNECTION, 1, NULL) == 0);
    CHECK(!(port_status() & (1U << HUB_PORT_FEATURE_C_CONNECTION)));
    CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_SET_FEATURE, HUB_PORT_FEATURE_RESET, 1,
                 NULL) == 0);
    CHECK(port_status() & (1U << HUB_PORT_FEATURE_ENABLE));
    for (int i = 0; i < g_nwk; i++) {
        wk_free(g_wk[i]);
    }
    g_nwk = 0;
    memset(&g_dev_stats, 0, sizeof(g_dev_stats));
    hport_init();
    enumerate(addr);
    g_stop = 0;
    start_wifi(4, 2);
}

static void traffic_flows(const char *when)
{
    uint64_t f0 = s_in.frames, o0 = g_dev_stats.out[1].frames;

    sim_dev_in_enqueue(0x81, 40, 1, 1600);
    pump_glue_us(60000);
    CHECKV(s_in.frames >= f0 + 40, "%s: %lu of 40 IN frames", when, (unsigned long)(s_in.frames - f0));
    CHECKV(g_dev_stats.out[1].frames > o0, "%s: no OUT frame", when);
    CHECKV(g_dev_stats.toggle_errors == 0, "%s: %lu toggle errors", when, (unsigned long)g_dev_stats.toggle_errors);
}

static void t_qa_hc_recover(void)
{
    struct wk *w = NULL;
    uint32_t r0;
    int rc;

    bringup();
    enumerate(50);
    start_wifi(4, 2);
    pump_glue_us(20000);
    traffic_flows("before");
    r0 = H()->stats.recoveries;

    /* 1) host system error: the controller halted itself */
    sim_hc_inject_hse();
    pump_glue_us(30000);
    printf("  HSE: hse %u recoveries %u (last %u ms), failures %u\n", H()->stats.hse, H()->stats.recoveries,
           H()->stats.last_recover_ms, H()->stats.recover_failures);
    CHECK(H()->stats.hse == 1);
    CHECKV(H()->stats.recoveries == r0 + 1, "recoveries %u", H()->stats.recoveries - r0);
    CHECK(sim_reg_peek(0x1a8) == 3); /* the glue tunables again (USBMODE host) */
    rehub_after_reset(51);
    traffic_flows("after HSE");

    /* 2) wedged core: AS/PS never follow; a kill still completes (deferred, then reset) */
    for (int i = 0; i < 4; i++) {
        if (w_in[i].busy) {
            w = &w_in[i];
            break;
        }
    }
    CHECK(w != NULL);
    g_sim_cfg.wedge = 1;
    if (w) {
        size_t f = usb_osal_enter_critical_section();
        w->want = 0;
        rc = usbh_kill_urb(&w->urb);
        usb_osal_leave_critical_section(f);
        printf("  wedge: kill rc %d, URB %s, misses %u, wedges %u, deferred %u, kill waits %u busy %u, "
               "recoveries %u\n", rc, w->busy ? "STILL IN THE CONTROLLER" : "back", H()->stats.handshake_timeouts,
               H()->stats.wedges, H()->stats.deferred, H()->stats.kill_waits, H()->stats.kill_busy,
               H()->stats.recoveries);
        CHECK(rc == 0 && !w->busy);
    }
    pump_glue_us(30000);
    CHECKV(H()->stats.recoveries == r0 + 2, "recoveries %u", H()->stats.recoveries - r0);
    CHECK(g_sim_cfg.wedge == 0);
    rehub_after_reset(52);
    traffic_flows("after wedge");

    /* 3) rate limit: a third death right away waits for the backoff */
    sim_hc_inject_hse();
    pump_glue_us(30000);
    CHECKV(H()->stats.recoveries == r0 + 2, "reset not delayed by the backoff (%u)", H()->stats.recoveries - r0);
    pump_glue_us(1500000);
    CHECKV(H()->stats.recoveries == r0 + 3, "reset never ran after the backoff (%u)", H()->stats.recoveries - r0);
    rehub_after_reset(53);
    traffic_flows("after backoff");
    check_accounting("recover");
    check_no_violations();
    close_all_and_check();
}

/* ================= recovery under load, unplug during reset, hotplug at a high rate ================= */
/*
 * Another thread running while the driver sleeps with the critical section released (handshake,
 * kill wait, controller reset): the hook runs at every driver sleep, never nested.
 */
void __real_usb_osal_msleep(uint32_t delay);
static void (*g_sleep_hook)(void);
static int g_in_sleep_hook;
static uint32_t g_hook_calls;

void __wrap_usb_osal_msleep(uint32_t delay)
{
    if (g_sleep_hook && !g_in_sleep_hook && g_sim_crit == 0) {
        g_in_sleep_hook = 1;
        g_hook_calls++;
        g_sleep_hook();
        g_in_sleep_hook = 0;
    }
    __real_usb_osal_msleep(delay);
}

static void stop_workers(void)
{
    g_stop = 1;
    for (int i = 0; i < g_nwk; i++) {
        g_wk[i]->want = 0;
    }
}

/* the hub thread after a controller reset or a reconnect: change seen, port reset, enumerate */
static int hub_reenumerate(uint8_t addr)
{
    uint32_t st = port_status();

    if (st & (1U << HUB_PORT_FEATURE_C_CONNECTION)) {
        CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_CLEAR_FEATURE,
                     HUB_PORT_FEATURE_C_CONNECTION, 1, NULL) == 0);
    }
    if (!(port_status() & (1U << HUB_PORT_FEATURE_CONNECTION))) {
        return 0;
    }
    CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_SET_FEATURE, HUB_PORT_FEATURE_RESET, 1,
                 NULL) == 0);
    for (int i = 0; i < g_nwk; i++) {
        wk_free(g_wk[i]);
    }
    g_nwk = 0;
    memset(&g_dev_stats, 0, sizeof(g_dev_stats));
    hport_init();
    enumerate(addr);
    g_stop = 0;
    return 1;
}

static void start_all(void)
{
    g_cc_expected = g_cc_got = g_ncp_pkts = g_evt_bad = 0;
    g_cc_head = g_cc_tail = 0;
    start_wifi(4, 2);
    sim_dev_in_enqueue(0x81, 1000000, 20, 1600);
    stream_init(&s_evt, 0x83, 0, 0);
    wk_init(&w_evt, &g_ep_evt, &s_evt, 4096);
    w_evt.rx = rx_events;
    w_evt.want = 1;
    wk_register(&w_evt);
    stream_init(&s_ain, 0x84, 0, 0);
    wk_init(&w_ain[0], &g_ep_acl_in, &s_ain, 4096);
    w_ain[0].want = 1;
    wk_register(&w_ain[0]);
    stream_init(&s_aout, 0x03, 300, 1000);
    wk_init(&w_aout, &g_ep_acl_out, &s_aout, 1024);
    w_aout.want = 0; /* one ACL frame per start (NCP events would pile up while workers stop) */
    wk_register(&w_aout);
    for (int i = 0; i < g_nwk; i++) {
        if (!g_wk[i]->busy) {
            wk_submit(g_wk[i]);
        }
    }
}

static int wait_recoveries(uint32_t n, uint32_t max_ms)
{
    for (uint32_t ms = 0; ms < max_ms && H()->stats.recoveries < n; ms++) {
        pump_glue_us(1000);
    }
    return H()->stats.recoveries >= n;
}

/* HSE / wedge at random phases of heavy Wi-Fi + BT traffic, 30 times */
static void t_qa_reset_under_load(void)
{
    uint32_t fails = 0;

    bringup();
    enumerate(60);
    start_all();
    pump_glue_us(20000);
    for (int it = 0; it < 30 && g_fail < 5; it++) {
        uint32_t r = H()->stats.recoveries;
        struct pools p;

        g_sim_cfg.stale_cache = sim_rand() & 1;
        g_dev_cfg.nak_permille = sim_rand() % 300;
        pump_glue_us(2000 + sim_rand() % 40000); /* any phase */
        if (it % 3 == 2) {
            /* wedged core found by a kill (timeout path) */
            struct wk *w = &w_in[sim_rand() % 4];
            g_sim_cfg.wedge = 1;
            sim_dev_block(0x81, 0); /* the killed URB cannot finish by itself (see qa_stuck_stop) */
            if (w->busy) {
                /* the adapter's loop: -USB_ERR_BUSY -> sleep with the lock released, again */
                int rc = 0, tries = 0;
                do {
                    size_t f = usb_osal_enter_critical_section();
                    int want = w->want;
                    w->want = 0;
                    rc = usbh_kill_urb(&w->urb);
                    w->want = want;
                    usb_osal_leave_critical_section(f);
                    if (rc == -USB_ERR_BUSY) {
                        pump_glue_us(1000);
                    }
                } while (rc == -USB_ERR_BUSY && w->busy && ++tries < 40000);
                CHECKV(!w->busy, "it %d: kill rc %d after %d BUSY retries, URB still in the controller", it, rc,
                       tries);
            } else {
                sim_hc_inject_hse();
            }
            sim_dev_block(0x81, -1);
        } else {
            sim_hc_inject_hse();
        }
        if (!wait_recoveries(r + 1, 40000)) {
            CHECKV(0, "it %d: no controller reset (USBCMD %08x USBSTS %08x USBINTR %08x running %d recovering %d "
                      "req %d gate %d in_irq %d pending %d after %d us, hse %u halted %u)", it, sim_reg_peek(0x140),
                   sim_reg_peek(0x144), sim_reg_peek(0x148), H()->running, H()->recovering, H()->recover_req,
                   H()->gate, H()->in_irq, H()->pending, (int)(H()->recover_after_us - sim_now_us()),
                   H()->stats.hse, H()->stats.halted_seen);
            break;
        }
        stop_workers();
        pump_glue_us(1000);
        p = pools_now();
        CHECKV(p.pipes == 0 && p.xfers == 0 && p.qtds_free == CONFIG_USB_EHCI_QTD_NUM && busy_workers() == 0,
               "it %d: after reset pipes %u xfers %u qtds %u busy %d", it, p.pipes, p.xfers, p.qtds_free,
               busy_workers());
        /* gated until the port reset */
        CHECK(ctrl(0x80, 6, 0x0100, 0, 8, 100) == -USB_ERR_NOTCONN);
        CHECK(hub_reenumerate((uint8_t)(61 + it)));
        start_all();
        traffic_flows("after reset under load");
        CHECKV(g_cc_got == g_cc_expected, "it %d HCI", it);
        if (sim_viol_total()) {
            check_no_violations();
            sim_viol_clear();
        }
        /* space the resets beyond the backoff window now and then */
        if (it % 5 == 4) {
            stop_workers();
            pump_glue_us(11000000);
            g_stop = 0;
            for (int i = 0; i < g_nwk; i++) {
                if (g_wk[i] != &w_aout) {
                    g_wk[i]->want = 1;
                }
            }
        }
    }
    printf("  reset under load: recoveries %u (HSE %u wedges %u stuck %u failures %u, last %u ms), kill waits %u "
           "busy %u, orphans %u, t=%.1f s\n",
           H()->stats.recoveries, H()->stats.hse, H()->stats.wedges, H()->stats.stuck_cancels,
           H()->stats.recover_failures, H()->stats.last_recover_ms, H()->stats.kill_waits, H()->stats.kill_busy,
           H()->stats.orphans, sim_now_ns() / 1e9);
    check_accounting("reset under load");
    check_no_violations();
    close_all_and_check();
}

/* unplug while the controller reset sleeps with the lock released; submit/kill from "other threads" */
static int g_hook_mode, g_hook_fired;
static uint32_t g_hook_refused, g_hook_accepted;
static struct usbh_urb g_hook_urb;
static uint8_t *g_hook_buf;

static void hook_during_reset(void)
{
    int rc;

    if (g_hook_fired || !H()->recovering) {
        return;
    }
    g_hook_fired = 1;
    if (g_hook_mode == 0) {
        /* the device is pulled; the hub thread releases it (kill with !connected) */
        sim_port_disconnect();
        g_hp.connected = false;
        {
            size_t f = usb_osal_enter_critical_section();
            CHECK(usbh_kill_urb(&g_ctl_urb) == 0);
            usb_osal_leave_critical_section(f);
        }
    }
    /* a class driver submits meanwhile: refused */
    memset(&g_hook_urb, 0, sizeof(g_hook_urb));
    usbh_bulk_urb_fill(&g_hook_urb, &g_hp, &g_ep_out_data, g_hook_buf, 100, 0, NULL, NULL);
    rc = usbh_submit_urb(&g_hook_urb);
    if (rc == -USB_ERR_NOTCONN) {
        g_hook_refused++;
    } else {
        g_hook_accepted++;
        CHECKV(0, "URB accepted during a controller reset (rc %d)", rc);
    }
    /* a kill of an URB the reset is handing back */
    {
        size_t f = usb_osal_enter_critical_section();
        for (int i = 0; i < 4; i++) {
            if (w_in[i].busy) {
                int k = usbh_kill_urb(&w_in[i].urb);
                CHECKV(k == 0 || k == -USB_ERR_BUSY || k == -USB_ERR_INVAL, "kill during reset rc %d", k);
                break;
            }
        }
        usb_osal_leave_critical_section(f);
    }
}

static void t_qa_unplug_during_reset(void)
{
    g_hook_buf = sim_dma_alloc(512, 32);
    for (int mode = 0; mode < 2; mode++) {
        uint32_t r;

        bringup();
        enumerate(70);
        start_all();
        pump_glue_us(20000);
        r = H()->stats.recoveries;
        g_hook_mode = mode;
        g_hook_fired = 0;
        g_sleep_hook = hook_during_reset;
        sim_hc_inject_hse();
        CHECK(wait_recoveries(r + 1, 5000));
        g_sleep_hook = NULL;
        CHECKV(g_hook_fired, "mode %d: the reset never slept with the lock released", mode);
        stop_workers();
        pump_glue_us(5000);
        {
            struct pools p = pools_now();
            CHECKV(p.pipes == 0 && p.xfers == 0 && p.qtds_free == CONFIG_USB_EHCI_QTD_NUM && busy_workers() == 0,
                   "mode %d: pipes %u xfers %u qtds %u busy %d", mode, p.pipes, p.xfers, p.qtds_free, busy_workers());
        }
        if (mode == 0) {
            /* replugged 300 ms later */
            pump_glue_us(300000);
            sim_port_connect();
            sim_advance_ms(2);
        }
        CHECK(hub_reenumerate((uint8_t)(71 + mode)));
        start_all();
        traffic_flows(mode ? "reset, hook submits" : "unplug during reset, replug");
        printf("  unplug during reset mode %d: hook refused %u accepted %u, recoveries %u failures %u\n", mode,
               g_hook_refused, g_hook_accepted, H()->stats.recoveries, H()->stats.recover_failures);
        check_accounting("unplug during reset");
        check_no_violations();
        close_all_and_check();
    }
    sim_dma_free(g_hook_buf);
}

/*
 * After a controller reset the hub thread resets the port (gate opens), waits
 * DELAY_TIME_AFTER_RESET (200 ms) and only then releases the old device. Meanwhile the old
 * class drivers (hport still connected, old address) may submit: those URBs go to an address
 * the reset device no longer has.
 */
static void t_qa_gate_stale_window(void)
{
    uint32_t r, accepted = 0, viol0;

    bringup();
    enumerate(80);
    start_all();
    pump_glue_us(20000);
    r = H()->stats.recoveries;
    sim_hc_inject_hse();
    CHECK(wait_recoveries(r + 1, 5000));
    stop_workers();
    pump_glue_us(1000);
    CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_CLEAR_FEATURE,
                 HUB_PORT_FEATURE_C_CONNECTION, 1, NULL) == 0);
    CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_SET_FEATURE, HUB_PORT_FEATURE_RESET, 1,
                 NULL) == 0);
    /* the old device's drivers, before usbh_hubport_release(): g_hp still connected, old address */
    viol0 = g_viol[V_ADDR];
    g_stop = 0;
    for (int i = 0; i < g_nwk; i++) {
        g_wk[i]->want = 1;
        if (!g_wk[i]->busy) {
            wk_submit(g_wk[i]);
            accepted += g_wk[i]->busy;
        }
    }
    pump_glue_us(200000);
    printf("  stale window: %u old-device URBs accepted after the port reset, %u transactions to the old address, "
           "errors %u\n", accepted, g_viol[V_ADDR] - viol0, H()->stats.errors);
    CHECKV(accepted == 0, "%u URBs of the released-to-be device accepted between port reset and release", accepted);
    stop_workers();
    /* the hub thread releases it now */
    g_hp.connected = false;
    {
        size_t f = usb_osal_enter_critical_section();
        usbh_kill_urb(&g_ctl_urb);
        usb_osal_leave_critical_section(f);
    }
    pump_glue_us(5000);
    CHECK(busy_workers() == 0);
    sim_viol_clear();
    for (int i = 0; i < g_nwk; i++) {
        wk_free(g_wk[i]);
    }
    g_nwk = 0;
    memset(&g_dev_stats, 0, sizeof(g_dev_stats));
    hport_init();
    enumerate(81);
    g_stop = 0;
    start_all();
    traffic_flows("after the stale window");
    check_no_violations();
    close_all_and_check();
}

/*
 * Wedged core (USBSTS.AS stops following USBCMD.ASE) found by a kill whose URB completes
 * during the stop handshake anyway: the kill returns at once (its URB is back), the stop stays
 * "stuck" (ASE off) and only ehci_nxp_service() retries it with 20 us waits. Expected: the
 * driver escalates to a controller reset (or the schedule runs again) within a bounded time.
 */
static void t_qa_stuck_stop(void)
{
    uint32_t r, fast0;
    uint64_t t0;
    int esc = 0;

    bringup();
    enumerate(95);
    start_wifi(4, 2);
    sim_dev_in_enqueue(0x81, 1000000, 20, 1600);
    pump_glue_us(20000);
    r = H()->stats.recoveries;
    g_sim_cfg.wedge = 1;
    for (int i = 0; i < 4; i++) {
        if (w_in[i].busy) {
            wk_kill(&w_in[i]);
            break;
        }
    }
    fast0 = g_glue_fast_wakeups;
    t0 = sim_now_ns();
    for (int ms = 0; ms < 10000 && !esc; ms++) {
        pump_glue_us(1000);
        esc = H()->stats.recoveries > r ||
              ((sim_reg_peek(0x140) & (1U << 5)) && !H()->pending && !H()->sched_stuck[0]);
    }
    printf("  stuck stop: escalated %s after %.0f ms; misses %u, handshake run %u, deferred %u, kill waits %u, "
           "glue 2 ms wakeups %u, USBCMD %08x pending %d stuck %d\n",
           esc ? "yes" : "NO", (sim_now_ns() - t0) / 1e6, H()->stats.handshake_timeouts, H()->hs_miss_run,
           H()->stats.deferred, H()->stats.kill_waits, g_glue_fast_wakeups - fast0, sim_reg_peek(0x140),
           H()->pending, H()->sched_stuck[0]);
    CHECKV(esc, "async schedule left disabled by a stuck stop with no controller reset for 10 s "
                "(ehci_nxp_service retries with 20 us waits never count as a miss)");
    g_sim_cfg.wedge = 0;
    sim_viol_clear();
    stop_workers();
    if (!esc) {
        size_t f = usb_osal_enter_critical_section();
        ehci_nxp_request_recover(B);
        usb_osal_leave_critical_section(f);
        wait_recoveries(r + 1, 35000);
    }
    pump_glue_us(5000);
    close_all_and_check();
}

/* USB-C hot swap: unplug/replug every 5..60 ms, also in the middle of enumeration and transfers */
static void t_qa_hotplug_rate(void)
{
    uint32_t enum_ok = 0, cycles = 0, mid_enum = 0;

    bringup();
    enumerate(90);
    start_all();
    for (int c = 0; c < 400 && g_fail < 5; c++) {
        uint32_t hold = 5 + sim_rand() % 55;
        struct pools p;

        pump_glue_us((uint64_t)hold * 1000);
        stop_workers();
        sim_port_disconnect();
        pump_glue_us(500 + sim_rand() % 3000);
        g_hp.connected = false;
        {
            size_t f = usb_osal_enter_critical_section();
            usbh_kill_urb(&g_ctl_urb);
            usb_osal_leave_critical_section(f);
        }
        pump_glue_us(2000);
        p = pools_now();
        CHECKV(p.pipes == 0 && p.xfers == 0 && p.qtds_free == CONFIG_USB_EHCI_QTD_NUM && busy_workers() == 0,
               "cycle %d: pipes %u xfers %u qtds %u busy %d", c, p.pipes, p.xfers, p.qtds_free, busy_workers());
        for (int i = 0; i < g_nwk; i++) {
            wk_free(g_wk[i]);
        }
        g_nwk = 0;
        sim_port_connect();
        pump_glue_us(1000 + sim_rand() % 5000);
        if (!(port_status() & (1U << HUB_PORT_FEATURE_CONNECTION))) {
            continue;
        }
        CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_CLEAR_FEATURE,
                     HUB_PORT_FEATURE_C_CONNECTION, 1, NULL) == 0);
        CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_SET_FEATURE,
                     HUB_PORT_FEATURE_RESET, 1, NULL) == 0);
        memset(&g_dev_stats, 0, sizeof(g_dev_stats));
        hport_init();
        if (c % 4 == 3) {
            /* pulled in the middle of enumeration */
            int n;
            CHECK(ctrl(0x80, 6, 0x0100, 0, 8, 500) == 8);
            sim_port_disconnect();
            n = ctrl(0x00, 5, (uint16_t)(1 + c % 100), 0, 0, 200);
            CHECKV(n < 0, "SET_ADDRESS to an unplugged device returned %d", n);
            g_hp.connected = false;
            {
                size_t f = usb_osal_enter_critical_section();
                usbh_kill_urb(&g_ctl_urb);
                usb_osal_leave_critical_section(f);
            }
            pump_glue_us(2000);
            CHECK(pools_now().pipes == 0 && pools_now().xfers == 0);
            sim_port_connect();
            pump_glue_us(2000);
            CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_CLEAR_FEATURE,
                         HUB_PORT_FEATURE_C_CONNECTION, 1, NULL) == 0);
            CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_SET_FEATURE,
                         HUB_PORT_FEATURE_RESET, 1, NULL) == 0);
            memset(&g_dev_stats, 0, sizeof(g_dev_stats));
            hport_init();
            mid_enum++;
        }
        enumerate((uint8_t)(1 + c % 120));
        enum_ok++;
        g_stop = 0;
        start_all();
        cycles++;
        if (sim_viol_total()) {
            printf("  cycle %d: violations\n", c);
            check_no_violations();
            sim_viol_clear();
        }
    }
    pump_glue_us(20000);
    traffic_flows("after hotplug storm");
    printf("  hotplug: %u cycles (%u pulled mid-enumeration), %u enumerations, recoveries %u, handshake misses %u, "
           "deferred %u, t=%.1f s\n",
           cycles, mid_enum, enum_ok, H()->stats.recoveries, H()->stats.handshake_timeouts, H()->stats.deferred,
           sim_now_ns() / 1e9);
    CHECK(H()->stats.recoveries == 0);
    check_accounting("hotplug");
    check_no_violations();
    close_all_and_check();
}

/* ================= runner ================= */
static const struct {
    const char *name;
    void (*fn)(void);
} k_qa[] = {
    { "qa_idle_60s", t_qa_idle_60s },
    { "qa_steady_cost", t_qa_steady_cost },
    { "qa_int_interval", t_qa_int_interval },
    { "qa_pages_zlp", t_qa_pages_zlp },
    { "qa_deferred_kill", t_qa_deferred_kill },
    { "qa_error_storm", t_qa_error_storm },
    { "qa_hc_recover", t_qa_hc_recover },
    { "qa_reset_under_load", t_qa_reset_under_load },
    { "qa_unplug_during_reset", t_qa_unplug_during_reset },
    { "qa_gate_stale_window", t_qa_gate_stale_window },
    { "qa_stuck_stop", t_qa_stuck_stop },
    { "qa_hotplug_rate", t_qa_hotplug_rate },
    { "qa_soak", t_qa_soak },
};

int main(int argc, char **argv)
{
    int failed = 0, ran = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    sim_init();
    signal(SIGALRM, on_alarm);
    for (unsigned i = 0; i < sizeof(k_qa) / sizeof(k_qa[0]); i++) {
        int sel = argc < 2;
        for (int a = 1; a < argc; a++) {
            if (strstr(k_qa[i].name, argv[a])) {
                sel = 1;
            }
        }
        if (!sel) {
            continue;
        }
        g_test = k_qa[i].name;
        g_fail = 0;
        printf("== %s\n", g_test);
        alarm(3600);
        k_qa[i].fn();
        alarm(0);
        printf("%s %s\n", g_fail ? "FAIL" : "PASS", g_test);
        failed += g_fail != 0;
        ran++;
    }
    printf("%d/%d qa tests passed\n", ran - failed, ran);
    return failed ? 1 : 0;
}
