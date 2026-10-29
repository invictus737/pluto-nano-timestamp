/*
 * AIC8800 D80-like device model (see sim_dev.h): EP0 control state machine with standard,
 * vendor and HCI class requests, bulk/interrupt endpoints with per-endpoint data toggles,
 * NAK until data is ready, random NAK/no-handshake, STALL, babble when the host offers less
 * room than the packet. Every check the device can make (address, endpoint, max packet,
 * toggle, frame contents and order) is reported as a violation or counted per endpoint.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_dev.h"
#include "sim_hc.h"

struct sim_dev_cfg g_dev_cfg = { .nak_permille = 0, .msg_latency_uf = 16, .hci_latency_uf = 16,
                                 .noresp_permille = 0, .acl_ncp_every = 4, .verbose = 0 };
struct sim_dev_stats g_dev_stats;

enum { T_CTRL, T_BULK, T_INT, T_ISO };
struct ep_def {
    uint8_t addr, type;
    uint16_t mps;
    uint8_t interval;
};
static const struct ep_def k_eps[] = {
    { 0x01, T_BULK, 512, 0 }, { 0x02, T_BULK, 512, 0 }, { 0x81, T_BULK, 512, 0 },
    { 0x82, T_BULK, 512, 0 }, { 0x83, T_INT, 16, 4 },   { 0x84, T_BULK, 512, 0 },
    { 0x03, T_BULK, 512, 0 }, { 0x85, T_ISO, 9, 1 },    { 0x04, T_ISO, 9, 1 },
};
#define NEPS (sizeof(k_eps) / sizeof(k_eps[0]))

static const struct ep_def *ep_find(uint8_t addr)
{
    for (unsigned i = 0; i < NEPS; i++) {
        if (k_eps[i].addr == addr) {
            return &k_eps[i];
        }
    }
    return NULL;
}

/* ---------------- descriptors ---------------- */
static const uint8_t k_dev_desc[18] = {
    18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 0x9C, 0xA6, 0x81, 0x8D, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t k_qual_desc[10] = { 10, 6, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 1, 0 };
static uint8_t g_cfg_desc[256];
static uint32_t g_cfg_len;

static void put_ep(uint8_t **p, const struct ep_def *e)
{
    uint8_t *d = *p;
    d[0] = 7;
    d[1] = 5;
    d[2] = e->addr;
    d[3] = e->type == T_BULK ? 2 : e->type == T_INT ? 3 : 1;
    d[4] = (uint8_t)e->mps;
    d[5] = (uint8_t)(e->mps >> 8);
    d[6] = e->type == T_BULK ? 0 : e->interval;
    *p += 7;
}

static void put_intf(uint8_t **p, uint8_t num, uint8_t alt, uint8_t neps, uint8_t cls, uint8_t sub,
                     uint8_t proto)
{
    uint8_t *d = *p;
    d[0] = 9;
    d[1] = 4;
    d[2] = num;
    d[3] = alt;
    d[4] = neps;
    d[5] = cls;
    d[6] = sub;
    d[7] = proto;
    d[8] = 0;
    *p += 9;
}

static void build_cfg(void)
{
    uint8_t *p = g_cfg_desc + 9;
    put_intf(&p, 0, 0, 4, 0xFF, 0xFF, 0xFF);
    for (int i = 0; i < 4; i++) {
        put_ep(&p, &k_eps[i]);
    }
    put_intf(&p, 1, 0, 3, 0xE0, 0x01, 0x01);
    put_ep(&p, &k_eps[4]);
    put_ep(&p, &k_eps[5]);
    put_ep(&p, &k_eps[6]);
    put_intf(&p, 2, 0, 0, 0xE0, 0x01, 0x01);
    put_intf(&p, 2, 1, 2, 0xE0, 0x01, 0x01);
    put_ep(&p, &k_eps[7]);
    put_ep(&p, &k_eps[8]);
    g_cfg_len = (uint32_t)(p - g_cfg_desc);
    uint8_t h[9] = { 9, 2, (uint8_t)g_cfg_len, (uint8_t)(g_cfg_len >> 8), 3, 1, 0, 0xE0, 250 };
    memcpy(g_cfg_desc, h, 9);
}

/* ---------------- device state ---------------- */
#define FQ 512
struct frame {
    uint16_t len;
    uint64_t ready_uf;
    uint8_t *data;
};
struct inq {
    struct frame f[FQ];
    uint32_t head, tail;     /* explicit frames */
    uint32_t gen_pending, gen_min, gen_max;
    uint32_t seq;            /* next seq the generator / responder uses */
    uint32_t off;            /* bytes of the head frame already sent */
    int zlp_due;
};
static struct inq g_inq[16];
static uint8_t g_outbuf[16][16384];
static uint32_t g_outlen[16];
static uint32_t g_out_seq[16];
static uint8_t g_dt_in[16], g_dt_out[16], g_halt_in[16], g_halt_out[16];
static uint64_t g_uf;
static uint32_t g_acl_out_frames;
static int32_t g_block[32]; /* per endpoint (addr & 15) + 16 for IN: packets still accepted, -1 free */

void sim_dev_block(uint8_t ep, int32_t packets)
{
    g_block[(ep & 15) + ((ep & 0x80) ? 16 : 0)] = packets;
}

uint32_t sim_dev_out_partial(uint8_t ep) { return g_outlen[ep & 15]; }
uint32_t sim_dev_in_partial(uint8_t ep) { return g_inq[ep & 15].off; }

static struct {
    int stage; /* 0 idle, 1 data in, 2 data out, 3 status in, 4 status out */
    uint8_t setup[8];
    uint8_t resp[1024];
    uint32_t resp_len, resp_off, out_len;
    uint8_t outbuf[1024];
    int stall, last_full, dt;
    uint8_t pending_addr;
    int have_pending_addr;
} g_c;

static void inq_clear(struct inq *q)
{
    while (q->head != q->tail) {
        free(q->f[q->head % FQ].data);
        q->head++;
    }
    q->head = q->tail = 0;
    q->off = 0;
    q->zlp_due = 0;
}

static void inq_push(uint8_t ep, const uint8_t *d, uint32_t len, uint64_t ready)
{
    struct inq *q = &g_inq[ep & 15];
    struct frame *f;
    if (q->tail - q->head >= FQ) {
        printf("    [dev] IN queue of ep %#x full, frame dropped\n", ep);
        return;
    }
    f = &q->f[q->tail % FQ];
    f->len = (uint16_t)len;
    f->ready_uf = ready;
    f->data = malloc(len ? len : 1);
    memcpy(f->data, d, len);
    q->tail++;
}

uint32_t sim_frame_wifi(uint8_t *buf, uint32_t len, uint32_t seq, uint8_t ep)
{
    if (len < 6) {
        len = 6;
    }
    buf[0] = (uint8_t)len;
    buf[1] = (uint8_t)(len >> 8);
    buf[2] = (uint8_t)seq;
    buf[3] = (uint8_t)(seq >> 8);
    buf[4] = ep;
    buf[5] = 0x5A;
    for (uint32_t i = 6; i < len; i++) {
        buf[i] = sim_pat(seq, i, ep);
    }
    return len;
}

uint32_t sim_frame_acl(uint8_t *buf, uint32_t payload, uint32_t seq, uint8_t ep)
{
    if (payload < 2) {
        payload = 2;
    }
    buf[0] = 0x01;
    buf[1] = 0x00;
    buf[2] = (uint8_t)payload;
    buf[3] = (uint8_t)(payload >> 8);
    buf[4] = (uint8_t)seq;
    buf[5] = (uint8_t)(seq >> 8);
    for (uint32_t i = 2; i < payload; i++) {
        buf[4 + i] = sim_pat(seq, i, ep);
    }
    return payload + 4;
}

static int is_acl_ep(uint8_t ep) { return (ep & 0x7F) == 3 || (ep & 0x7F) == 4; }

int64_t sim_frame_check(const uint8_t *b, uint32_t len, uint8_t ep)
{
    if (is_acl_ep(ep)) {
        uint32_t pl;
        if (len < 6) {
            return -1;
        }
        pl = b[2] | (b[3] << 8);
        if (pl + 4 != len || b[0] != 1 || b[1] != 0) {
            return -1;
        }
        for (uint32_t i = 2; i < pl; i++) {
            if (b[4 + i] != sim_pat(b[4] | (b[5] << 8), i, ep)) {
                return -1;
            }
        }
        return b[4] | (b[5] << 8);
    }
    if (len < 6 || (uint32_t)(b[0] | (b[1] << 8)) != len || b[4] != ep || b[5] != 0x5A) {
        return -1;
    }
    {
        uint32_t seq = b[2] | (b[3] << 8);
        for (uint32_t i = 6; i < len; i++) {
            if (b[i] != sim_pat(seq, i, ep)) {
                return -1;
            }
        }
        return seq;
    }
}

static uint32_t rnd_len(uint32_t lo, uint32_t hi)
{
    return hi > lo ? lo + sim_rand() % (hi - lo + 1) : lo;
}

void sim_dev_in_enqueue(uint8_t ep, uint32_t n, uint32_t minlen, uint32_t maxlen)
{
    struct inq *q = &g_inq[ep & 15];
    q->gen_pending += n;
    q->gen_min = minlen;
    q->gen_max = maxlen;
}

uint32_t sim_dev_in_pending(uint8_t ep)
{
    struct inq *q = &g_inq[ep & 15];
    return q->gen_pending + (q->tail - q->head);
}

uint32_t sim_dev_in_next_seq(uint8_t ep) { return g_inq[ep & 15].seq; }
uint32_t sim_dev_out_next_seq(uint8_t ep) { return g_out_seq[ep & 15]; }

void sim_dev_hci_event(const uint8_t *evt, uint32_t len)
{
    inq_push(0x83, evt, len, g_uf);
}

void sim_dev_halt_ep(uint8_t ep)
{
    if (ep & 0x80) {
        g_halt_in[ep & 15] = 1;
    } else {
        g_halt_out[ep & 15] = 1;
    }
}

static void gen_refill(uint8_t ep)
{
    struct inq *q = &g_inq[ep & 15];
    static uint8_t tmp[16384];
    while (q->gen_pending && q->tail - q->head < 8) {
        uint32_t len;
        if (is_acl_ep(ep)) {
            len = sim_frame_acl(tmp, rnd_len(q->gen_min, q->gen_max), q->seq, ep);
        } else {
            len = sim_frame_wifi(tmp, rnd_len(q->gen_min, q->gen_max), q->seq, ep);
        }
        q->seq++;
        q->gen_pending--;
        inq_push(ep, tmp, len, g_uf + (sim_rand() % 4));
    }
}

void sim_dev_bus_reset(void)
{
    g_dev_stats.addr = 0;
    g_dev_stats.config = 0;
    memset(&g_c, 0, sizeof(g_c));
    for (int i = 0; i < 16; i++) {
        inq_clear(&g_inq[i]);
        g_inq[i].gen_pending = 0;
        g_inq[i].seq = 0;
        g_outlen[i] = 0;
        g_out_seq[i] = 0;
        g_dt_in[i] = g_dt_out[i] = g_halt_in[i] = g_halt_out[i] = 0;
    }
    g_acl_out_frames = 0;
    for (int i = 0; i < 32; i++) {
        g_block[i] = -1;
    }
    if (!g_cfg_len) {
        build_cfg();
    }
}

void sim_dev_detach(void)
{
    sim_dev_bus_reset();
}

void sim_dev_tick_uframe(void)
{
    g_uf++;
    gen_refill(0x81);
    gen_refill(0x84);
}

/* ---------------- frame reception ---------------- */
static void hci_command_done(const uint8_t *cmd, uint32_t len)
{
    uint8_t ev[6];
    uint16_t op = len >= 2 ? (uint16_t)(cmd[0] | (cmd[1] << 8)) : 0;
    g_dev_stats.hci_cmds++;
    ev[0] = 0x0E; /* Command Complete */
    ev[1] = 4;
    ev[2] = 1;
    ev[3] = (uint8_t)op;
    ev[4] = (uint8_t)(op >> 8);
    ev[5] = 0;
    inq_push(0x83, ev, 6, g_uf + 1 + sim_rand() % (g_dev_cfg.hci_latency_uf + 1));
}

static void out_frame_done(uint8_t ep)
{
    int e = ep & 15;
    int64_t seq = sim_frame_check(g_outbuf[e], g_outlen[e], ep);
    struct sim_ep_stats *st = &g_dev_stats.out[e];

    st->frames++;
    st->bytes += g_outlen[e];
    if (seq < 0) {
        st->bad++;
        if (st->bad <= 3) {
            printf("    [dev] ep %#x: corrupt OUT frame of %u bytes (hdr %02x %02x %02x %02x)\n", ep,
                   g_outlen[e], g_outbuf[e][0], g_outbuf[e][1], g_outbuf[e][2], g_outbuf[e][3]);
        }
    } else if ((uint32_t)seq != (g_out_seq[e] & 0xFFFF)) {
        st->dup_or_gap++;
        if (st->dup_or_gap <= 3) {
            printf("    [dev] ep %#x: OUT frame seq %u, expected %u (lost/duplicated)\n", ep,
                   (unsigned)seq, g_out_seq[e] & 0xFFFF);
        }
        g_out_seq[e] = (uint32_t)seq + 1;
    } else {
        g_out_seq[e]++;
    }
    if (ep == 0x02 && seq >= 0) {
        /* message: answer on 0x82 */
        static uint8_t r[1024];
        uint32_t len = sim_frame_wifi(r, 64 + (uint32_t)(seq % 200), g_inq[2].seq++, 0x82);
        inq_push(0x82, r, len, g_uf + 1 + sim_rand() % (g_dev_cfg.msg_latency_uf + 1));
    }
    if (ep == 0x03 && g_dev_cfg.acl_ncp_every && ++g_acl_out_frames % g_dev_cfg.acl_ncp_every == 0) {
        uint8_t ev[7] = { 0x13, 5, 1, 0x01, 0x00, (uint8_t)g_dev_cfg.acl_ncp_every, 0 };
        inq_push(0x83, ev, 7, g_uf + 2);
    }
    g_outlen[e] = 0;
}

/* ---------------- control endpoint ---------------- */
static void ctrl_setup(const uint8_t *s)
{
    uint8_t bmrt = s[0], req = s[1];
    uint16_t wv = (uint16_t)(s[2] | (s[3] << 8)), wl = (uint16_t)(s[6] | (s[7] << 8));
    const uint8_t *r = NULL;
    uint32_t rl = 0;
    static uint8_t tmp[64];

    memcpy(g_c.setup, s, 8);
    g_c.stall = 0;
    g_c.resp_off = 0;
    g_c.out_len = 0;
    g_c.last_full = 0;
    g_c.dt = 1;
    g_dev_stats.setups++;
    if (bmrt & 0x80) {
        if ((bmrt & 0x60) == 0 && req == 6) {
            switch (wv >> 8) {
            case 1: r = k_dev_desc; rl = 18; break;
            case 2: r = g_cfg_desc; rl = g_cfg_len; break;
            case 3:
                if ((wv & 0xFF) == 0) {
                    static const uint8_t lang[4] = { 4, 3, 0x09, 0x04 };
                    r = lang;
                    rl = 4;
                } else {
                    static const uint8_t str[8] = { 8, 3, 'A', 0, 'I', 0, 'C', 0 };
                    r = str;
                    rl = 8;
                }
                break;
            case 6: r = k_qual_desc; rl = 10; break;
            default: g_c.stall = 1; break;
            }
        } else if ((bmrt & 0x60) == 0 && req == 8) {
            tmp[0] = g_dev_stats.config;
            r = tmp;
            rl = 1;
        } else if ((bmrt & 0x60) == 0 && req == 0) {
            memset(tmp, 0, 2);
            r = tmp;
            rl = 2;
        } else if ((bmrt & 0x60) == 0x40 && req != 0xFF) {
            memset(tmp, 0x11, sizeof(tmp));
            r = tmp;
            rl = wl < 64 ? wl : 64;
        } else {
            g_c.stall = 1;
        }
        if (rl > wl) {
            rl = wl;
        }
        if (r) {
            memcpy(g_c.resp, r, rl);
        }
        g_c.resp_len = rl;
        g_c.stage = wl ? 1 : 3;
    } else {
        if ((bmrt & 0x60) == 0x40 && req == 0xFF) {
            g_c.stall = 1;
        }
        g_c.stage = wl ? 2 : 3;
    }
    if (g_c.stall) {
        g_dev_stats.stalls_sent++;
    }
}

static void ctrl_status_done(void)
{
    uint8_t bmrt = g_c.setup[0], req = g_c.setup[1];
    uint16_t wv = (uint16_t)(g_c.setup[2] | (g_c.setup[3] << 8));
    uint16_t wi = (uint16_t)(g_c.setup[4] | (g_c.setup[5] << 8));

    if (bmrt == 0x00 && req == 5) {
        g_dev_stats.addr = (uint8_t)(wv & 0x7F);
    } else if (bmrt == 0x00 && req == 9) {
        g_dev_stats.config = (uint8_t)wv;
        for (int i = 1; i < 16; i++) {
            g_dt_in[i] = g_dt_out[i] = 0;
            g_halt_in[i] = g_halt_out[i] = 0;
        }
    } else if (bmrt == 0x02 && req == 1 && wv == 0) {
        if (wi & 0x80) {
            g_dt_in[wi & 15] = 0;
            g_halt_in[wi & 15] = 0;
        } else {
            g_dt_out[wi & 15] = 0;
            g_halt_out[wi & 15] = 0;
        }
    } else if (bmrt == 0x01 && req == 11) {
        /* SET_INTERFACE: isochronous alternates, nothing to do */
    } else if ((bmrt & 0x60) == 0x20 && req == 0) {
        hci_command_done(g_c.outbuf, g_c.out_len);
    }
    g_c.stage = 0;
}

static enum sim_hs ctrl_transact(uint8_t pid, int toggle, uint8_t *buf, uint32_t *len)
{
    if (pid == 2) {
        if (*len != 8) {
            sim_viol(V_MISC, "SETUP with %u bytes", *len);
        }
        if (toggle) {
            sim_viol(V_TOGGLE, "SETUP sent with DATA1 (must be DATA0)");
        }
        ctrl_setup(buf);
        return HS_ACK;
    }
    if (g_c.stage == 0) {
        sim_viol(V_MISC, "EP0 %s without a SETUP", pid == 1 ? "IN" : "OUT");
        return HS_STALL;
    }
    if (g_c.stall) {
        return HS_STALL;
    }
    {
        /* status stage: always DATA1 (USB 2.0 8.5.3) */
        int status = (pid == 0 && g_c.stage == 1) || (pid == 1 && (g_c.stage == 2 || g_c.stage == 3));
        if (status && pid == 1 && g_c.stage == 2 &&
            g_c.out_len < (uint32_t)(g_c.setup[6] | (g_c.setup[7] << 8))) {
            status = 1;
        }
        if (status) {
            g_c.dt = 1;
        }
    }
    if (toggle != g_c.dt) {
        g_dev_stats.toggle_errors++;
        sim_viol(V_TOGGLE, "EP0 %s stage %d with DATA%d, device expects DATA%d (request %02x %02x)",
                 pid == 1 ? "IN" : "OUT", g_c.stage, toggle, g_c.dt, g_c.setup[0], g_c.setup[1]);
        if (pid == 1) {
            return HS_NAK;
        }
        *len = 0;
        return HS_ACK;
    }
    if (pid == 1) {
        if (g_c.stage == 1) {
            uint32_t left = g_c.resp_len - g_c.resp_off, n = left < 64 ? left : 64;
            if (n > *len) {
                return HS_BABBLE;
            }
            memcpy(buf, g_c.resp + g_c.resp_off, n);
            g_c.resp_off += n;
            *len = n;
            g_c.dt ^= 1;
            return HS_ACK;
        }
        if (g_c.stage == 3 || g_c.stage == 2) {
            if (g_c.stage == 2 && g_c.out_len < (uint32_t)(g_c.setup[6] | (g_c.setup[7] << 8))) {
                sim_viol(V_MISC, "control write status stage after %u of %u data bytes", g_c.out_len,
                         g_c.setup[6] | (g_c.setup[7] << 8));
            }
            *len = 0;
            ctrl_status_done();
            return HS_ACK;
        }
        sim_viol(V_MISC, "EP0 IN in stage %d", g_c.stage);
        return HS_STALL;
    }
    /* OUT */
    if (g_c.stage == 2) {
        if (*len > 64) {
            sim_viol(V_MPS, "EP0 OUT packet of %u bytes", *len);
        }
        if (g_c.out_len + *len <= sizeof(g_c.outbuf)) {
            memcpy(g_c.outbuf + g_c.out_len, buf, *len);
        }
        g_c.out_len += *len;
        g_c.dt ^= 1;
        return HS_ACK;
    }
    if (g_c.stage == 1) {
        /* status OUT of a control read */
        if (*len != 0) {
            sim_viol(V_MISC, "control read status OUT with %u bytes", *len);
        }
        ctrl_status_done();
        return HS_ACK;
    }
    sim_viol(V_MISC, "EP0 OUT in stage %d", g_c.stage);
    return HS_STALL;
}

/* ---------------- transactions ---------------- */
enum sim_hs sim_dev_transact(uint8_t addr, uint8_t ep, uint8_t pid, int toggle, uint8_t *buf,
                             uint32_t *len, uint16_t mps, int is_periodic)
{
    const struct ep_def *e;
    uint8_t epa;
    struct sim_ep_stats *st;

    if (!sim_dev_present()) {
        return HS_NORESP;
    }
    if (addr != g_dev_stats.addr) {
        sim_viol(V_ADDR, "transaction to address %u, device is at %u", addr, g_dev_stats.addr);
        return HS_NORESP;
    }
    if (ep == 0) {
        if (mps != 64) {
            sim_viol(V_MPS, "EP0 QH max packet %u, device bMaxPacketSize0 64", mps);
        }
        if (is_periodic) {
            sim_viol(V_MISC, "EP0 on the periodic schedule");
        }
        if (pid != 2 && g_dev_cfg.noresp_permille && sim_rand() % 1000 < g_dev_cfg.noresp_permille) {
            return HS_NORESP;
        }
        return ctrl_transact(pid, toggle, buf, len);
    }
    if (pid == 2) {
        sim_viol(V_MISC, "SETUP to endpoint %u", ep);
        return HS_NORESP;
    }
    epa = (uint8_t)(ep | (pid == 1 ? 0x80 : 0));
    e = ep_find(epa);
    if (!e || g_dev_stats.config == 0) {
        sim_viol(V_ADDR, "transaction to endpoint %#x %s", epa, e ? "before SET_CONFIGURATION" : "that does not exist");
        return HS_NORESP;
    }
    if (mps != e->mps) {
        sim_viol(V_MPS, "QH max packet %u for endpoint %#x (wMaxPacketSize %u)", mps, epa, e->mps);
    }
    if ((e->type == T_INT) != (is_periodic != 0)) {
        sim_viol(V_MISC, "endpoint %#x (%s) on the %s schedule", epa, e->type == T_INT ? "interrupt" : "bulk",
                 is_periodic ? "periodic" : "asynchronous");
    }
    st = pid == 1 ? &g_dev_stats.in[ep] : &g_dev_stats.out[ep];
    if (pid == 1 ? g_halt_in[ep] : g_halt_out[ep]) {
        g_dev_stats.stalls_sent++;
        return HS_STALL;
    }
    if (g_dev_cfg.noresp_permille && sim_rand() % 1000 < g_dev_cfg.noresp_permille) {
        return HS_NORESP;
    }
    {
        int32_t *b = &g_block[ep + (pid == 1 ? 16 : 0)];
        if (*b == 0) {
            st->naks++;
            return HS_NAK;
        }
    }
    if (e->type == T_BULK && g_dev_cfg.nak_permille && sim_rand() % 1000 < g_dev_cfg.nak_permille) {
        st->naks++;
        return HS_NAK;
    }
    if (pid == 0) {
        if (*len > e->mps) {
            sim_viol(V_MPS, "OUT packet of %u bytes on endpoint %#x (max %u)", *len, epa, e->mps);
        }
        if (toggle != g_dt_out[ep]) {
            /* the device takes it for a retransmission: ACK, data discarded */
            g_dev_stats.toggle_errors++;
            sim_viol(V_TOGGLE, "OUT %#x with DATA%d, device expects DATA%d: %u bytes discarded", epa,
                     toggle, g_dt_out[ep], *len);
            return HS_ACK;
        }
        g_dt_out[ep] ^= 1;
        st->packets++;
        if (g_block[ep] > 0) {
            g_block[ep]--;
        }
        if (g_outlen[ep] + *len <= sizeof(g_outbuf[0])) {
            memcpy(g_outbuf[ep] + g_outlen[ep], buf, *len);
        }
        g_outlen[ep] += *len;
        if (*len < e->mps) {
            out_frame_done(epa);
        }
        return HS_ACK;
    }
    /* IN */
    {
        struct inq *q = &g_inq[ep];
        struct frame *f;
        uint32_t n;

        if (q->head == q->tail || q->f[q->head % FQ].ready_uf > g_uf) {
            st->naks++;
            return HS_NAK;
        }
        f = &q->f[q->head % FQ];
        if (q->zlp_due) {
            n = 0;
        } else {
            n = f->len - q->off;
            if (n > e->mps) {
                n = e->mps;
            }
        }
        if (n > *len) {
            return HS_BABBLE; /* the host offered less room than the packet */
        }
        if (toggle != g_dt_in[ep]) {
            /* host ACKs and drops a packet with the wrong PID: the device moves on, data lost */
            g_dev_stats.toggle_errors++;
            sim_viol(V_TOGGLE, "IN %#x: device sends DATA%d, host overlay expects DATA%d: %u bytes lost",
                     epa, g_dt_in[ep], toggle, n);
        } else {
            memcpy(buf, f->data + q->off, n);
        }
        g_dt_in[ep] ^= 1;
        st->packets++;
        if (g_block[ep + 16] > 0) {
            g_block[ep + 16]--;
        }
        q->off += n;
        if (q->zlp_due || (n < e->mps)) {
            q->zlp_due = 0;
        } else if (q->off == f->len) {
            q->zlp_due = 1;
        }
        if (q->off == f->len && !q->zlp_due) {
            st->frames++;
            st->bytes += f->len;
            free(f->data);
            q->head++;
            q->off = 0;
        }
        if (toggle != (g_dt_in[ep] ^ 1)) {
            return HS_NAK;
        }
        *len = n;
        return HS_ACK;
    }
}
