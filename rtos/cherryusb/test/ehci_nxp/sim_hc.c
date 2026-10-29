/*
 * EHCI 1.0 controller model, ChipIdea (Zynq-7000 USB0) register layout.
 *
 * What it models (EHCI 1.0 section numbers):
 *  - registers: USBCMD (RS, HCRESET self-clearing, FS/FS2, PSE, ASE, IAAD doorbell software
 *    cannot clear, ITC), USBSTS write-1-to-clear (UI UEI PCI FRI SEI AAI SRI + ChipIdea UAI UPI
 *    TI0 TI1) with HCH/RCL/PS/AS read-only, USBINTR, FRINDEX, PERIODICLISTBASE, ASYNCLISTADDR
 *    (updated by the HC as it walks), CONFIGFLAG, PORTSC1 (CSC/PEC/OCC W1C, PE clear-only,
 *    ChipIdea PR self-clearing and PSPD), USBMODE, GPTIMER0/1 (ChipIdea general purpose
 *    timers, TI0/TI1), ULPI viewport (always idle).
 *  - 2.3.1/4.8: USBSTS.AS/PS follow ASE/PSE after a delay (lag mode: random, long); changing
 *    ASE/PSE before the status followed is a violation.
 *  - 4.8/4.10: async schedule walk from ASYNCLISTADDR along QH horizontal links, the H-bit
 *    head and reclamation (an idle lap stops the walk until the next microframe), NAK counter
 *    reload, overlay load from next/alternate next qTD (alt when the last qTD was short),
 *    data toggle from the overlay (DTC=0) or from the qTD (DTC=1), short packets, Total Bytes,
 *    C_Page/offset over five 4 KiB pages, CERR/XactErr retry and halt, STALL and babble halt,
 *    IOC, qTD token write-back.
 *  - 4.8.2: Interrupt on Async Advance: the doorbell is answered after the HC passed the head
 *    (and a configurable delay); the HC may hold the QH it is parked on and follow its
 *    horizontal link afterwards. "stale_cache" mode: it keeps a full copy of that QH and keeps
 *    executing from the copy until the doorbell is answered or the async schedule stops.
 *  - 4.6/4.12: periodic frame list (size from FS/FS2), FRINDEX microframe walk, interrupt QHs
 *    selected by S-mask, iTD/siTD/FSTN links skipped.
 *  - 4.15: interrupt threshold (ITC) for USBINT/USBERRINT, IAA and PCI immediate.
 *
 * Checks (violations, sim_hc.h): pointers, alignment, ring shape, handshakes, reuse of QHs and
 * qTDs the HC may hold, edits of active overlays, toggles, cache maintenance of data buffers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "sim_hc.h"

void sim_mmio_init(void);
int extern_rand_mod(uint32_t m);
uint32_t sim_rand(void);

struct sim_cfg g_sim_cfg;
static const struct sim_cfg k_cfg_default = {
    .mmio_cost_ns = 200,
    .as_delay_uf = 1,
    .as_lag_max_uf = 0,
    .iaa_delay_uf = 1,
    .iaa_need_head = 1,
    .iaa_when_async_off = 1,
    .stale_cache = 0,
    .async_budget = 8,
    .irq_latency_uf = 0,
    .preempt = 1,
    .port_reset_ms = 20,
    .seed = 1,
};
struct sim_stats g_sim_stats;
uint32_t g_viol[V_COUNT];
int g_sim_crit, g_sim_in_isr, g_sim_in_mmio;

const char *const g_viol_name[V_COUNT] = {
    "PTR_RANGE", "PTR_ALIGN", "ASYNC_LINK", "ASE_HANDSHAKE", "ASYNCADDR_LIVE",
    "PERIODICBASE_LIVE", "QH_REUSE_CACHED", "QTD_REUSE_HELD", "OVERLAY_TOUCH", "TOGGLE", "MPS",
    "ADDR", "EPS", "DMA_DIRTY", "IAA_ASYNC_OFF", "RS_MODE", "CPAGE", "PERIODIC_LINK",
    "SLEEP_IN_CRIT", "QTD_ACTIVE_HALT", "MISC",
};

static uint32_t g_viol_printed;

void sim_viol(enum sim_viol v, const char *fmt, ...)
{
    va_list ap;

    g_viol[v]++;
    if (g_viol[v] <= 3 && g_viol_printed < 60) {
        g_viol_printed++;
        printf("    [sim t=%.3fms] VIOLATION %s: ", (double)sim_now_ns() / 1e6, g_viol_name[v]);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
}

uint32_t sim_viol_total(void)
{
    uint32_t n = 0;
    for (int i = 0; i < V_COUNT; i++) {
        n += g_viol[i];
    }
    return n;
}

void sim_viol_clear(void)
{
    memset(g_viol, 0, sizeof(g_viol));
    g_viol_printed = 0;
}

void sim_viol_dump(void)
{
    for (int i = 0; i < V_COUNT; i++) {
        if (g_viol[i]) {
            printf("    violations %-18s %u\n", g_viol_name[i], g_viol[i]);
        }
    }
}

/* ---------------- random ---------------- */
static uint32_t g_rng = 1;
uint32_t sim_rand(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
void sim_srand(uint32_t s) { g_rng = s ? s : 1; }

/* ---------------- memory ---------------- */
#define ARENA_BASE 0x20000000UL
#define ARENA_SIZE (64UL << 20)
extern char __executable_start[];
extern char _end[];

struct blk {
    uint32_t off, size;
    uint8_t used;
};
#define MAX_BLK 16384
static struct blk g_blk[MAX_BLK];
static uint32_t g_nblk;
static size_t g_in_use;
static uint32_t g_allocs;

static void arena_init(void)
{
    void *p = mmap((void *)ARENA_BASE, ARENA_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)ARENA_BASE) {
        perror("sim: DMA arena");
        exit(2);
    }
    g_blk[0].off = 0;
    g_blk[0].size = ARENA_SIZE;
    g_blk[0].used = 0;
    g_nblk = 1;
}

void *sim_dma_alloc(size_t size, size_t align)
{
    if (align < 32) {
        align = 32;
    }
    size = (size + 31) & ~(size_t)31;
    if (size == 0) {
        size = 32;
    }
    for (uint32_t i = 0; i < g_nblk; i++) {
        struct blk *b = &g_blk[i];
        uint32_t start, pad;
        if (b->used) {
            continue;
        }
        start = (uint32_t)((b->off + align - 1) & ~(align - 1));
        pad = start - b->off;
        if (b->size < pad + size) {
            continue;
        }
        if (g_nblk + 2 >= MAX_BLK) {
            fprintf(stderr, "sim: allocator table full\n");
            exit(2);
        }
        if (pad) { /* split off the padding as a free block */
            memmove(&g_blk[i + 1], &g_blk[i], (g_nblk - i) * sizeof(*b));
            g_nblk++;
            g_blk[i].size = pad;
            i++;
            b = &g_blk[i];
            b->off = start;
            b->size -= pad;
        }
        if (b->size > size) {
            memmove(&g_blk[i + 1], &g_blk[i], (g_nblk - i) * sizeof(*b));
            g_nblk++;
            g_blk[i + 1].off = b->off + (uint32_t)size;
            g_blk[i + 1].size = b->size - (uint32_t)size;
            g_blk[i + 1].used = 0;
            b->size = (uint32_t)size;
        }
        b->used = 1;
        g_in_use += size;
        g_allocs++;
        memset((void *)(ARENA_BASE + b->off), 0xA5, size); /* not zeroed, like a heap */
        return (void *)(ARENA_BASE + b->off);
    }
    return NULL;
}

void sim_dma_free(void *p)
{
    uint32_t off;

    if (p == NULL) {
        return;
    }
    off = (uint32_t)((uintptr_t)p - ARENA_BASE);
    for (uint32_t i = 0; i < g_nblk; i++) {
        if (g_blk[i].off == off && g_blk[i].used) {
            g_blk[i].used = 0;
            g_in_use -= g_blk[i].size;
            g_allocs--;
            memset(p, 0xDB, g_blk[i].size); /* poison: a use after free shows up */
            if (i + 1 < g_nblk && !g_blk[i + 1].used) {
                g_blk[i].size += g_blk[i + 1].size;
                memmove(&g_blk[i + 1], &g_blk[i + 2], (g_nblk - i - 2) * sizeof(g_blk[0]));
                g_nblk--;
            }
            if (i > 0 && !g_blk[i - 1].used) {
                g_blk[i - 1].size += g_blk[i].size;
                memmove(&g_blk[i], &g_blk[i + 1], (g_nblk - i - 1) * sizeof(g_blk[0]));
                g_nblk--;
            }
            return;
        }
    }
    fprintf(stderr, "sim: free of %p that is not an arena block\n", p);
    sim_viol(V_MISC, "free of %p that is not an arena block", p);
}

size_t sim_dma_in_use(void) { return g_in_use; }
uint32_t sim_dma_allocs(void) { return g_allocs; }

int sim_addr_ok(uint32_t addr, uint32_t len)
{
    uintptr_t a = addr, e = (uintptr_t)addr + len;

    if (a >= (uintptr_t)__executable_start && e <= (uintptr_t)_end) {
        return 1;
    }
    if (a >= ARENA_BASE && e <= ARENA_BASE + ARENA_SIZE) {
        return 1;
    }
    return 0;
}

static inline uint32_t m32(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline void w32(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }

/* ---------------- cache model of tracked data buffers ---------------- */
struct cbuf {
    uintptr_t base;
    size_t len;
    int is_in;
    uint8_t *dirty; /* per 32-byte line */
};
#define MAX_CBUF 256
static struct cbuf g_cbuf[MAX_CBUF];

void sim_cache_track(void *buf, size_t len, int is_in)
{
    for (int i = 0; i < MAX_CBUF; i++) {
        if (g_cbuf[i].base == 0) {
            g_cbuf[i].base = (uintptr_t)buf;
            g_cbuf[i].len = len;
            g_cbuf[i].is_in = is_in;
            g_cbuf[i].dirty = calloc((len + 31) / 32 + 1, 1);
            return;
        }
    }
}

void sim_cache_untrack(void *buf)
{
    for (int i = 0; i < MAX_CBUF; i++) {
        if (g_cbuf[i].base == (uintptr_t)buf) {
            free(g_cbuf[i].dirty);
            memset(&g_cbuf[i], 0, sizeof(g_cbuf[i]));
        }
    }
}

static void cache_lines(uintptr_t addr, size_t len, int op /* 0 clean,1 inval,2 dirty */)
{
    for (int i = 0; i < MAX_CBUF; i++) {
        struct cbuf *c = &g_cbuf[i];
        uintptr_t s, e;
        if (!c->base || addr + len <= c->base || addr >= c->base + c->len) {
            continue;
        }
        s = addr < c->base ? c->base : addr;
        e = addr + len > c->base + c->len ? c->base + c->len : addr + len;
        for (uintptr_t l = (s - c->base) / 32; l <= (e - 1 - c->base) / 32; l++) {
            if (op == 1 && c->dirty[l] && !c->is_in) {
                sim_viol(V_DMA_DIRTY, "invalidate discards dirty CPU data of OUT buffer %#lx line %lu",
                         (unsigned long)c->base, (unsigned long)l);
            }
            c->dirty[l] = op == 2;
        }
    }
}

void sim_cache_cpu_wrote(void *buf, size_t len) { cache_lines((uintptr_t)buf, len, 2); }
void sim_cache_clean(uintptr_t addr, size_t len) { cache_lines(addr, len, 0); }
void sim_cache_invalidate(uintptr_t addr, size_t len) { cache_lines(addr, len, 1); }

static void cache_dma_check(uint32_t addr, uint32_t len, int write)
{
    for (int i = 0; i < MAX_CBUF; i++) {
        struct cbuf *c = &g_cbuf[i];
        if (!c->base || addr + len <= c->base || addr >= c->base + c->len || len == 0) {
            continue;
        }
        uintptr_t s = addr < c->base ? c->base : addr;
        uintptr_t e = addr + len > c->base + c->len ? c->base + c->len : addr + len;
        for (uintptr_t l = (s - c->base) / 32; l <= (e - 1 - c->base) / 32; l++) {
            if (c->dirty[l]) {
                sim_viol(V_DMA_DIRTY, "DMA %s %#x+%u hits a dirty line of buffer %#lx (no %s)",
                         write ? "write" : "read", addr, len, (unsigned long)c->base,
                         write ? "clean/invalidate before an IN transfer" : "clean before an OUT transfer");
                c->dirty[l] = 0;
            }
        }
    }
}

/* ---------------- registers ---------------- */
/* offsets from the controller base (0xE0002000 on the Zynq) */
#define R_GPT0LD    0x080
#define R_GPT0CTRL  0x084
#define R_GPT1LD    0x088
#define R_GPT1CTRL  0x08C
#define R_CAPLEN    0x100
#define R_HCSPARAMS 0x104
#define R_HCCPARAMS 0x108
#define R_DCIVER    0x120
#define R_DCCPARAMS 0x124
#define R_USBCMD    0x140
#define R_USBSTS    0x144
#define R_USBINTR   0x148
#define R_FRINDEX   0x14C
#define R_PLBASE    0x154
#define R_ALADDR    0x158
#define R_ULPI      0x170
#define R_CONFIGFLAG 0x180
#define R_PORTSC    0x184
#define R_OTGSC     0x1A4
#define R_USBMODE   0x1A8

#define CMD_RS    (1U << 0)
#define CMD_RST   (1U << 1)
#define CMD_PSE   (1U << 4)
#define CMD_ASE   (1U << 5)
#define CMD_IAAD  (1U << 6)
#define CMD_FS2   (1U << 15)

#define STS_UI   (1U << 0)
#define STS_UEI  (1U << 1)
#define STS_PCI  (1U << 2)
#define STS_FRI  (1U << 3)
#define STS_SEI  (1U << 4)
#define STS_AAI  (1U << 5)
#define STS_SRI  (1U << 7)
#define STS_HCH  (1U << 12)
#define STS_RCL  (1U << 13)
#define STS_PS   (1U << 14)
#define STS_AS   (1U << 15)
#define STS_UAI  (1U << 18)
#define STS_UPI  (1U << 19)
#define STS_TI0  (1U << 24)
#define STS_TI1  (1U << 25)
#define STS_W1C  (STS_UI | STS_UEI | STS_PCI | STS_FRI | STS_SEI | STS_AAI | (1U << 6) | STS_SRI | \
                  (1U << 8) | (1U << 10) | (1U << 16) | STS_UAI | STS_UPI | STS_TI0 | STS_TI1)

#define PS_CCS  (1U << 0)
#define PS_CSC  (1U << 1)
#define PS_PE   (1U << 2)
#define PS_PEC  (1U << 3)
#define PS_OCC  (1U << 5)
#define PS_FPR  (1U << 6)
#define PS_SUSP (1U << 7)
#define PS_PR   (1U << 8)
#define PS_HSP  (1U << 9)
#define PS_PP   (1U << 12)
#define PS_PSPD_HS (2U << 26)
#define PS_PSPD_MASK (3U << 26)
#define PS_W1C  (PS_CSC | PS_PEC | PS_OCC)
#define PS_RO   (PS_CCS | (1U << 4) | (3U << 10) | PS_HSP | PS_PSPD_MASK)

static struct {
    uint32_t usbcmd, usbsts, usbintr, frindex, plbase, aladdr, configflag, portsc, usbmode, otgsc;
    uint32_t gptld[2], gptctrl[2], gptcnt[2];
    uint32_t raw[1024];
} R;

static uint64_t g_now, g_next_uf, g_gpt_last_us;
static uint64_t g_rst_done_ns, g_port_reset_done_ns, g_ase_due_ns, g_pse_due_ns, g_halt_due_ns;
static int g_dev_present;
static uint32_t g_pending_int; /* USBINT/USBERRINT etc. waiting for the ITC boundary */
static uint64_t g_irq_up_ns;

/* async engine state */
static uint32_t g_cur;         /* QH the async engine is parked on (0: none) */
static uint32_t g_ring_seq, g_answered_seq;
static uint64_t g_ring_ns;
static int g_passed_head;
static struct {
    int valid;
    uint32_t addr;
    uint32_t dw[12];
} g_cache;
static uint32_t g_as_epoch = 1; /* increments whenever the async engine stops */
static uint32_t g_ps_epoch = 1; /* ... and the periodic one */

/* what the HC last saw of every QH (to detect software edits of things it may hold) */
struct seen {
    uint32_t addr;
    uint32_t dw[12];
    uint32_t epoch;          /* g_as_epoch at the time */
    uint32_t unlink_seq;     /* doorbell sequence when it was seen unlinked (0: linked) */
    uint32_t qtd;            /* qTD the overlay was loaded from, its token at load */
    uint32_t qtd_token, qtd_next;
    int qtd_owned;           /* HC owns it: loaded/restarted and not yet written back done */
};
#define SEEN_N 512
static struct seen g_seen[SEEN_N];

static struct seen *seen_get(uint32_t addr, int create)
{
    uint32_t h = (addr >> 5) % SEEN_N;
    for (uint32_t i = 0; i < SEEN_N; i++) {
        struct seen *s = &g_seen[(h + i) % SEEN_N];
        if (s->addr == addr) {
            return s;
        }
        if (s->addr == 0) {
            if (!create) {
                return NULL;
            }
            memset(s, 0, sizeof(*s));
            s->addr = addr;
            return s;
        }
    }
    return NULL;
}

uint64_t sim_now_ns(void) { return g_now; }
uint32_t sim_hc_current_qh(void) { return g_cur; }
uint32_t sim_now_us(void) { return (uint32_t)(g_now / 1000); }

static uint32_t fl_size(void)
{
    uint32_t fs = ((R.usbcmd >> 2) & 3) | ((R.usbcmd & CMD_FS2) ? 4 : 0);
    return 1024U >> fs;
}

static void reset_regs(void)
{
    uint32_t keep_portsc_dev = g_dev_present;

    memset(&R, 0, sizeof(R));
    R.usbcmd = 0x00080000; /* ITC 8 */
    R.usbsts = STS_HCH;
    R.portsc = (2U << 30); /* PTS = ULPI, PP off (HCSPARAMS.PPC) */
    R.usbmode = 0;         /* idle: CM must be written again */
    R.raw[R_CAPLEN / 4] = 0x01000040;
    R.raw[R_HCSPARAMS / 4] = 0x00010011;
    R.raw[R_HCCPARAMS / 4] = 0x00000006;
    R.raw[R_DCIVER / 4] = 0x00000001;
    R.raw[R_DCCPARAMS / 4] = 0x0000018C;
    (void)keep_portsc_dev;
    g_cur = 0;
    g_cache.valid = 0;
    g_pending_int = 0;
    g_as_epoch++;
    g_ps_epoch++;
    g_ase_due_ns = g_pse_due_ns = g_halt_due_ns = 0;
    g_port_reset_done_ns = 0;
}

static void port_update_connect(void)
{
    uint32_t ccs = (g_dev_present && (R.portsc & PS_PP)) ? PS_CCS : 0;
    if ((R.portsc & PS_CCS) != ccs) {
        R.portsc = (R.portsc & ~PS_CCS) | ccs | PS_CSC;
        if (!ccs && (R.portsc & PS_PE)) {
            R.portsc &= ~(PS_PE | PS_PSPD_MASK | PS_HSP);
            R.portsc |= PS_PEC;
        }
        R.usbsts |= STS_PCI;
    }
}

void sim_dev_bus_reset(void);
void sim_dev_detach(void);

static void cache_drop(void);

void sim_hc_inject_hse(void)
{
    /* EHCI 2.3.2: a host system error clears Run/Stop; the controller halts at once */
    R.usbcmd &= ~CMD_RS;
    R.usbsts |= STS_SEI | STS_HCH;
    R.usbsts &= ~(STS_AS | STS_PS);
    g_cur = 0;
    cache_drop();
    g_as_epoch++;
    g_ps_epoch++;
    g_halt_due_ns = 0;
}

void sim_port_connect(void)
{
    g_dev_present = 1;
    sim_dev_bus_reset();
    port_update_connect();
}

void sim_port_disconnect(void)
{
    g_dev_present = 0;
    sim_dev_detach();
    g_port_reset_done_ns = 0;
    R.portsc &= ~PS_PR;
    port_update_connect();
}

int sim_dev_present(void) { return g_dev_present && (R.portsc & PS_PE) && !(R.usbsts & STS_HCH); }

static void gpt_update(void)
{
    uint64_t us = g_now / 1000;
    uint64_t d = us - g_gpt_last_us;

    g_gpt_last_us = us;
    for (int n = 0; n < 2; n++) {
        uint64_t left = d;
        while ((R.gptctrl[n] & (1U << 31)) && left) {
            uint64_t step = left < R.gptcnt[n] ? left : R.gptcnt[n];
            R.gptcnt[n] -= (uint32_t)step;
            left -= step;
            if (R.gptcnt[n] == 0) {
                R.usbsts |= n ? STS_TI1 : STS_TI0;
                if (R.gptctrl[n] & (1U << 24)) {
                    R.gptcnt[n] = (R.gptld[n] & 0xFFFFFF) + 1;
                } else {
                    R.gptctrl[n] &= ~(1U << 31);
                }
            }
            if (step == 0) {
                break;
            }
        }
    }
}

static void hc_uframe(void);

static void deliver_irq(void)
{
    const uint32_t mask = 0x030D05FFU;

    if (g_sim_in_mmio || g_sim_in_isr || g_sim_crit) {
        return;
    }
    for (int n = 0; n < 8 && (R.usbsts & R.usbintr & mask); n++) {
        if (g_irq_up_ns == 0) {
            g_irq_up_ns = g_now;
        }
        if (g_now - g_irq_up_ns < (uint64_t)g_sim_cfg.irq_latency_uf * 125000) {
            return;
        }
        g_sim_in_isr++;
        g_sim_stats.irq_calls++;
        sim_irq_handler();
        g_sim_in_isr--;
    }
    g_irq_up_ns = 0;
}

void sim_preempt_point(void)
{
    if (g_sim_cfg.preempt) {
        deliver_irq();
    }
}

void sim_advance_ns(uint64_t ns)
{
    uint64_t target = g_now + ns;

    while (g_next_uf <= target) {
        g_now = g_next_uf;
        g_next_uf += 125000;
        gpt_update();
        hc_uframe();
        deliver_irq();
    }
    g_now = target;
    gpt_update();
}

void sim_advance_us(uint32_t us) { sim_advance_ns((uint64_t)us * 1000); }
void sim_advance_ms(uint32_t ms) { sim_advance_ns((uint64_t)ms * 1000000); }

int sim_run_until(int (*cond)(void *), void *arg, uint32_t timeout_ms)
{
    uint64_t end = g_now + (uint64_t)timeout_ms * 1000000;
    while (g_now < end) {
        if (cond(arg)) {
            return 1;
        }
        sim_advance_ns(125000);
    }
    return cond(arg);
}

uint32_t sim_reg_peek(uint32_t off)
{
    switch (off & ~3U) {
    case R_USBCMD: return R.usbcmd;
    case R_USBSTS: return R.usbsts;
    case R_USBINTR: return R.usbintr;
    case R_FRINDEX: return R.frindex;
    case R_PLBASE: return R.plbase;
    case R_ALADDR: return R.aladdr;
    case R_CONFIGFLAG: return R.configflag;
    case R_PORTSC: return R.portsc;
    case R_OTGSC: return R.otgsc;
    case R_USBMODE: return R.usbmode;
    case R_GPT0LD: return R.gptld[0];
    case R_GPT1LD: return R.gptld[1];
    case R_GPT0CTRL: return (R.gptctrl[0] & 0xC1000000U & ~(1U << 30)) | (R.gptcnt[0] & 0xFFFFFF);
    case R_GPT1CTRL: return (R.gptctrl[1] & 0xC1000000U & ~(1U << 30)) | (R.gptcnt[1] & 0xFFFFFF);
    default: return R.raw[(off & 0xFFF) / 4];
    }
}

uint32_t sim_reg_read(uint32_t off, unsigned size)
{
    uint32_t v;

    sim_advance_ns(g_sim_cfg.mmio_cost_ns);
    v = sim_reg_peek(off);
    if (size < 4) {
        v >>= (off & 3) * 8;
        v &= size == 1 ? 0xFF : 0xFFFF;
    }
    return v;
}

static uint32_t ring_mask_snapshot_unlinked(void);

static void write_usbcmd(uint32_t v)
{
    uint32_t old = R.usbcmd;

    if (v & CMD_RST) {
        if (!(R.usbsts & STS_HCH)) {
            sim_viol(V_RS_MODE, "HCRESET written while the controller runs (HCH=0)");
        }
        reset_regs();
        g_sim_cfg.wedge = 0; /* a reset core answers handshakes again */
        R.usbcmd |= CMD_RST;
        g_rst_done_ns = g_now + 2000;
        return;
    }
    if ((v & CMD_RS) && !(old & CMD_RS) && (R.usbmode & 3) != 3) {
        sim_viol(V_RS_MODE, "RS set with USBMODE.CM=%u (not host)", R.usbmode & 3);
    }
    if ((v ^ old) & CMD_ASE) {
        int ase = (old & CMD_ASE) != 0, as = (R.usbsts & STS_AS) != 0;
        if (ase != as) {
            sim_viol(V_ASE_HANDSHAKE, "ASE %d->%d while USBSTS.AS=%d (EHCI 4.8: wait for AS == ASE)",
                     ase, !ase, as);
        }
        g_sim_stats.ase_toggles++;
        g_ase_due_ns = g_now + (uint64_t)(g_sim_cfg.as_delay_uf +
                       (g_sim_cfg.as_lag_max_uf ? (extern_rand_mod(g_sim_cfg.as_lag_max_uf + 1)) : 0)) * 125000;
    }
    if ((v ^ old) & CMD_PSE) {
        int pse = (old & CMD_PSE) != 0, ps = (R.usbsts & STS_PS) != 0;
        if (pse != ps) {
            sim_viol(V_ASE_HANDSHAKE, "PSE %d->%d while USBSTS.PS=%d", pse, !pse, ps);
        }
        g_sim_stats.pse_toggles++;
        g_pse_due_ns = g_now + (uint64_t)(g_sim_cfg.as_delay_uf +
                       (g_sim_cfg.as_lag_max_uf ? (extern_rand_mod(g_sim_cfg.as_lag_max_uf + 1)) : 0)) * 125000;
    }
    if ((v & CMD_IAAD) && !(old & CMD_IAAD)) {
        if (!(R.usbsts & STS_AS)) {
            sim_viol(V_IAA_ASYNC_OFF, "doorbell rung with USBSTS.AS=0%s",
                     g_sim_cfg.iaa_when_async_off ? "" : " (strict: never answered)");
        }
        (void)ring_mask_snapshot_unlinked();
        g_ring_seq++;
        g_ring_ns = g_now;
        g_passed_head = 0;
        g_sim_stats.iaa_rings++;
    }
    if (old & CMD_IAAD) {
        v |= CMD_IAAD; /* software cannot clear the doorbell */
    }
    if (!(v & CMD_RS) && (old & CMD_RS)) {
        g_halt_due_ns = g_now + 125000;
    }
    if ((v & CMD_RS) && !(old & CMD_RS)) {
        R.usbsts &= ~STS_HCH;
    }
    R.usbcmd = v & ~CMD_RST;
}

static void write_portsc(uint32_t v)
{
    uint32_t old = R.portsc, n;

    n = (old & (PS_RO | PS_W1C | PS_PE)) | (v & ~(PS_RO | PS_W1C | PS_PE));
    n &= ~(v & PS_W1C);
    if (!(v & PS_PE)) {
        n &= ~PS_PE; /* software may disable, never enable */
    }
    if ((v & PS_PR) && !(old & PS_PR)) {
        if (!(old & PS_CCS)) {
            n &= ~PS_PR; /* nothing attached: no reset */
        } else {
            n &= ~(PS_PE | PS_HSP | PS_PSPD_MASK);
            g_port_reset_done_ns = g_now + (uint64_t)g_sim_cfg.port_reset_ms * 1000000;
            sim_dev_bus_reset();
        }
    } else if (!(v & PS_PR) && (old & PS_PR)) {
        n |= PS_PR; /* ChipIdea times the reset itself */
    }
    R.portsc = n;
    if ((n ^ old) & PS_PP) {
        port_update_connect();
    }
}

static void write_gptctrl(int t, uint32_t v)
{
    if (v & (1U << 30)) {
        R.gptcnt[t] = (R.gptld[t] & 0xFFFFFF) + 1;
    }
    R.gptctrl[t] = v & 0x81000000U;
}

void sim_reg_write(uint32_t off, uint32_t val, unsigned size)
{
    uint32_t a = off & ~3U;

    sim_advance_ns(g_sim_cfg.mmio_cost_ns);
    if (size < 4) {
        uint32_t sh = (off & 3) * 8, m = (size == 1 ? 0xFFU : 0xFFFFU) << sh;
        uint32_t cur = sim_reg_peek(a);
        if (a == R_USBSTS) {
            cur = 0; /* W1C: untouched bytes write zeros */
        }
        val = (cur & ~m) | ((val << sh) & m);
    }
    switch (a) {
    case R_USBCMD:
        write_usbcmd(val);
        break;
    case R_USBSTS:
        R.usbsts &= ~(val & STS_W1C);
        break;
    case R_USBINTR:
        R.usbintr = val;
        break;
    case R_FRINDEX:
        if (!(R.usbsts & STS_HCH)) {
            sim_viol(V_MISC, "FRINDEX written while running");
        }
        R.frindex = val & 0x3FFF;
        break;
    case R_PLBASE:
        if ((val & 0xFFF) != 0) {
            sim_viol(V_PTR_ALIGN, "PERIODICLISTBASE %#x not 4 KiB aligned", val);
        }
        if ((R.usbsts & STS_PS) && val != R.plbase) {
            sim_viol(V_PERIODICBASE_LIVE, "PERIODICLISTBASE changed with PS=1");
        }
        R.plbase = val & ~0xFFFU;
        break;
    case R_ALADDR:
        if ((R.usbsts & STS_AS) || (R.usbcmd & CMD_ASE)) {
            sim_viol(V_ASYNCADDR_LIVE, "ASYNCLISTADDR written (%#x) with ASE=%d AS=%d", val,
                     !!(R.usbcmd & CMD_ASE), !!(R.usbsts & STS_AS));
        }
        if (val & 0x1F) {
            sim_viol(V_PTR_ALIGN, "ASYNCLISTADDR %#x not 32-byte aligned", val);
        }
        R.aladdr = val & ~0x1FU;
        break;
    case R_CONFIGFLAG:
        R.configflag = val & 1;
        break;
    case R_PORTSC:
        write_portsc(val);
        break;
    case R_OTGSC:
        R.otgsc = (R.otgsc & ~(val & 0x007F0000U)) | (val & 0x7F00002FU);
        break;
    case R_USBMODE:
        R.usbmode = val;
        break;
    case R_GPT0LD:
        R.gptld[0] = val & 0xFFFFFF;
        break;
    case R_GPT1LD:
        R.gptld[1] = val & 0xFFFFFF;
        break;
    case R_GPT0CTRL:
        write_gptctrl(0, val);
        break;
    case R_GPT1CTRL:
        write_gptctrl(1, val);
        break;
    case R_ULPI:
        R.raw[a / 4] = val & ~(1U << 30); /* ULPIRUN completes at once */
        break;
    case R_CAPLEN:
    case R_HCSPARAMS:
    case R_HCCPARAMS:
    case R_DCIVER:
    case R_DCCPARAMS:
        break; /* read-only */
    default:
        R.raw[(a & 0xFFF) / 4] = val;
        break;
    }
}

/* ---------------- QH / qTD helpers ---------------- */
#define TOK_PING   (1U << 0)
#define TOK_XACT   (1U << 3)
#define TOK_BABBLE (1U << 4)
#define TOK_DBE    (1U << 5)
#define TOK_HALT   (1U << 6)
#define TOK_ACTIVE (1U << 7)
#define TOK_PID(t) (((t) >> 8) & 3)
#define TOK_CERR(t) (((t) >> 10) & 3)
#define TOK_CPAGE(t) (((t) >> 12) & 7)
#define TOK_IOC    (1U << 15)
#define TOK_BYTES(t) (((t) >> 16) & 0x7FFF)
#define TOK_DT     (1U << 31)

#define EPC_ADDR(e) ((e) & 0x7F)
#define EPC_EP(e) (((e) >> 8) & 0xF)
#define EPC_EPS(e) (((e) >> 12) & 3)
#define EPC_DTC (1U << 14)
#define EPC_H (1U << 15)
#define EPC_MPS(e) (((e) >> 16) & 0x7FF)
#define EPC_RL(e) (((e) >> 28) & 0xF)

static int qh_read(uint32_t a, uint32_t dw[12], const char *why)
{
    if (a & 0x1F) {
        sim_viol(V_PTR_ALIGN, "QH %#x not 32-byte aligned (%s)", a, why);
        return -1;
    }
    if (!sim_addr_ok(a, 48)) {
        sim_viol(V_PTR_RANGE, "QH pointer %#x outside DMA memory (%s)", a, why);
        return -1;
    }
    for (int i = 0; i < 12; i++) {
        dw[i] = m32(a + 4 * i);
    }
    return 0;
}

static void qh_write_overlay(uint32_t a, const uint32_t dw[12])
{
    for (int i = 3; i < 12; i++) {
        w32(a + 4 * i, dw[i]);
    }
}

/* walk the async ring from its head in memory; returns count, -1 if malformed */
static uint32_t g_head;

static int ring_walk(uint32_t *out, int max, uint32_t *head_out)
{
    uint32_t start = g_cur ? g_cur : R.aladdr, a, head = 0;

    if (g_head && sim_addr_ok(g_head, 48) && (m32(g_head + 4) & EPC_H)) {
        start = g_head;
    }
    a = start;
    int n = 0, guard = 0;

    if (!start) {
        return 0;
    }
    /* find the head first */
    while (guard++ < 300) {
        if (!sim_addr_ok(a, 48) || (a & 0x1F)) {
            return -1;
        }
        if (m32(a + 4) & EPC_H) {
            head = a;
            break;
        }
        uint32_t l = m32(a);
        if ((l & 1) || ((l >> 1) & 3) != 1) {
            return -1;
        }
        a = l & ~0x1FU;
        if (a == start) {
            return -1; /* a ring without head */
        }
    }
    if (!head) {
        return -1;
    }
    a = head;
    guard = 0;
    do {
        uint32_t l;
        if (out && n < max) {
            out[n] = a;
        }
        n++;
        l = m32(a);
        if ((l & 1) || ((l >> 1) & 3) != 1 || !sim_addr_ok(l & ~0x1FU, 48)) {
            return -1;
        }
        a = l & ~0x1FU;
    } while (a != head && guard++ < 300);
    if (a != head) {
        return -1;
    }
    if (head_out) {
        *head_out = head;
    }
    g_head = head;
    return n;
}

static int in_ring(uint32_t qh)
{
    uint32_t r[300];
    int n = ring_walk(r, 300, NULL);
    for (int i = 0; i < n && i < 300; i++) {
        if (r[i] == qh) {
            return 1;
        }
    }
    return 0;
}

/* called when the doorbell is rung: held QHs already out of the ring are covered by it */
static uint32_t ring_mask_snapshot_unlinked(void)
{
    uint32_t held[2] = { g_cur, g_cache.valid ? g_cache.addr : 0 };
    for (int i = 0; i < 2; i++) {
        struct seen *s;
        if (!held[i] || in_ring(held[i])) {
            continue;
        }
        s = seen_get(held[i], 1);
        if (s && s->unlink_seq == 0) {
            s->unlink_seq = g_ring_seq + 1; /* covered by the doorbell being rung now */
        }
    }
    return 0;
}

uint32_t sim_async_ring(uint32_t *out, uint32_t max)
{
    int n = ring_walk(out, (int)max, NULL);
    return n < 0 ? 0 : (uint32_t)n;
}

int extern_rand_mod(uint32_t m) { return (int)(sim_rand() % m); }

/* ---------------- transactions ---------------- */
static uint8_t g_pkt[4096];

/* gather/scatter packet bytes across the five buffer pages of an overlay */
static int buf_xfer(uint32_t dw[12], uint32_t len, int to_mem, const uint8_t *src, uint8_t *dst)
{
    uint32_t tok = dw[6];
    uint32_t cpage = TOK_CPAGE(tok), off = dw[7] & 0xFFF, done = 0;

    while (done < len) {
        uint32_t page, chunk, addr;
        if (cpage > 4) {
            sim_viol(V_CPAGE, "transfer runs past buffer page 4 (qTD %#x)", dw[3]);
            return -1;
        }
        page = dw[7 + cpage] & ~0xFFFU;
        addr = page | off;
        chunk = 4096 - off;
        if (chunk > len - done) {
            chunk = len - done;
        }
        if (!sim_addr_ok(addr, chunk)) {
            sim_viol(V_PTR_RANGE, "data buffer %#x+%u outside DMA memory (qTD %#x page %u)", addr,
                     chunk, dw[3], cpage);
            return -1;
        }
        cache_dma_check(addr, chunk, to_mem);
        if (to_mem) {
            memcpy((void *)(uintptr_t)addr, src + done, chunk);
        } else {
            memcpy(dst + done, (void *)(uintptr_t)addr, chunk);
        }
        done += chunk;
        off += chunk;
        if (off >= 4096) {
            off = 0;
            cpage++;
        }
    }
    return 0;
}

static void buf_advance(uint32_t dw[12], uint32_t len)
{
    uint32_t cpage = TOK_CPAGE(dw[6]), off = (dw[7] & 0xFFF) + len;
    cpage += off / 4096;
    off %= 4096;
    dw[6] = (dw[6] & ~(7U << 12)) | ((cpage & 7) << 12);
    dw[7] = (dw[7] & ~0xFFFU) | off;
}

static void raise_int(uint32_t bits, int periodic)
{
    g_pending_int |= bits;
    if (bits & STS_UI) {
        g_pending_int |= periodic ? STS_UPI : STS_UAI;
    }
}

/* write back the token of the qTD the overlay came from; check software left it alone */
static void qtd_writeback(uint32_t qhaddr, uint32_t dw[12])
{
    uint32_t q = dw[3];
    struct seen *s = seen_get(qhaddr, 1);

    if (!q || !sim_addr_ok(q, 32)) {
        sim_viol(V_PTR_RANGE, "write-back to qTD %#x outside DMA memory (QH %#x)", q, qhaddr);
        return;
    }
    if (s && s->qtd == q && s->qtd_owned) {
        uint32_t memtok = m32(q + 8);
        if (memtok != s->qtd_token) {
            sim_viol(V_QTD_REUSE_HELD,
                     "qTD %#x (QH %#x) token changed by software from %#x to %#x while the HC "
                     "held it in the overlay; write-back %#x overwrites it",
                     q, qhaddr, s->qtd_token, memtok, dw[6]);
        }
        s->qtd_token = dw[6];
    }
    if (s) {
        s->qtd_owned = 0; /* done or halted: software owns qTD and overlay again */
    }
    w32(q + 8, dw[6]);
}

static int g_progress;

/* one transaction on the queue whose overlay is dw; returns 1 if a packet used the bus */
static int qh_transact(uint32_t qa, uint32_t dw[12], int periodic)
{
    uint32_t tok = dw[6], epc = dw[1];
    uint32_t pid = TOK_PID(tok), total = TOK_BYTES(tok), mps = EPC_MPS(epc), len;
    int toggle = (tok & TOK_DT) != 0, done = 0;
    enum sim_hs hs;

    {
        /* (re)acquire: a qTD software restarted in a halted/idle overlay is the HC's again */
        struct seen *s = seen_get(qa, 1);
        if (s && (!s->qtd_owned || s->qtd != dw[3])) {
            s->qtd = dw[3];
            s->qtd_token = sim_addr_ok(dw[3], 32) ? m32(dw[3] + 8) : 0;
            s->qtd_next = sim_addr_ok(dw[3], 32) ? m32(dw[3]) : 0;
            s->qtd_owned = 1;
        }
    }
    if (EPC_EPS(epc) != 2) {
        sim_viol(V_EPS, "QH %#x endpoint speed %u for a high-speed device", qa, EPC_EPS(epc));
    }
    if (pid == 3) {
        sim_viol(V_MISC, "qTD %#x with reserved PID code 3", dw[3]);
        dw[6] = (tok & ~TOK_ACTIVE) | TOK_HALT;
        qh_write_overlay(qa, dw);
        qtd_writeback(qa, dw);
        raise_int(STS_UEI, periodic);
        return 0;
    }
    if (pid == 2 && total != 8) {
        sim_viol(V_MISC, "SETUP qTD %#x with %u bytes", dw[3], total);
    }
    if (pid == 1) {
        len = total; /* room the qTD offers; the device babbles past it */
    } else {
        len = total < mps ? total : mps;
        if (buf_xfer(dw, len, 0, NULL, g_pkt) < 0) {
            dw[6] = (tok & ~TOK_ACTIVE) | TOK_HALT | TOK_DBE;
            qh_write_overlay(qa, dw);
            qtd_writeback(qa, dw);
            raise_int(STS_UEI, periodic);
            return 0;
        }
    }
    hs = sim_dev_transact(EPC_ADDR(epc), EPC_EP(epc), (uint8_t)pid, toggle, g_pkt, &len, (uint16_t)mps,
                          periodic);
    g_sim_stats.packets++;
    switch (hs) {
    case HS_NAK:
        g_sim_stats.naks++;
        if (!periodic && EPC_RL(epc)) {
            uint32_t nc = (dw[5] >> 1) & 0xF;
            if (nc) {
                nc--;
            }
            dw[5] = (dw[5] & ~0x1EU) | (nc << 1);
            qh_write_overlay(qa, dw);
        }
        return 1;
    case HS_STALL:
        g_sim_stats.stalls++;
        g_sim_stats.halts++;
        dw[6] = (tok & ~TOK_ACTIVE) | TOK_HALT;
        done = 2;
        break;
    case HS_NORESP: {
        uint32_t cerr = TOK_CERR(tok);
        g_sim_stats.xacterrs++;
        tok |= TOK_XACT;
        if (cerr) {
            cerr--;
            tok = (tok & ~(3U << 10)) | (cerr << 10);
            if (cerr == 0) {
                tok = (tok & ~TOK_ACTIVE) | TOK_HALT;
                g_sim_stats.halts++;
                done = 2;
            }
        }
        dw[6] = tok;
        if (!done) {
            qh_write_overlay(qa, dw);
            return 1;
        }
        break;
    }
    case HS_BABBLE:
        g_sim_stats.babbles++;
        g_sim_stats.halts++;
        dw[6] = (tok & ~TOK_ACTIVE) | TOK_HALT | TOK_BABBLE;
        done = 2;
        break;
    case HS_ACK:
        g_progress = 1;
        if (pid == 1) {
            if (len > total) {
                g_sim_stats.babbles++;
                g_sim_stats.halts++;
                dw[6] = (tok & ~TOK_ACTIVE) | TOK_HALT | TOK_BABBLE;
                done = 2;
                break;
            }
            if (len && buf_xfer(dw, len, 1, g_pkt, NULL) < 0) {
                dw[6] = (tok & ~TOK_ACTIVE) | TOK_HALT | TOK_DBE;
                done = 2;
                break;
            }
        }
        total -= len;
        buf_advance(dw, len);
        tok = dw[6];
        tok = (tok & ~(0x7FFFU << 16)) | (total << 16);
        tok ^= TOK_DT;
        if (TOK_CERR(dw[6]) != TOK_CERR(tok)) {
            /* unchanged */
        }
        if (pid == 1 && len < mps) {
            g_sim_stats.shorts++;
            done = 1;
            if (total) {
                raise_int(STS_UI, periodic); /* short packet detect (EHCI 2.3.2) */
            }
        }
        if (total == 0) {
            done = 1;
        }
        if (done) {
            tok &= ~TOK_ACTIVE;
        }
        dw[6] = tok;
        break;
    }
    if (done) {
        if (done == 2) {
            raise_int(STS_UEI, periodic);
        } else if (dw[6] & TOK_IOC) {
            raise_int(STS_UI, periodic);
        }
    }
    qh_write_overlay(qa, dw);
    if (done) {
        qtd_writeback(qa, dw);
    }
    return 1;
}

/* EHCI 4.10.2 advance queue: load the overlay from the next (or alternate) qTD */
static int qh_advance(uint32_t qa, uint32_t dw[12])
{
    uint32_t tok = dw[6], nxt, q[8];
    struct seen *s;

    if (TOK_BYTES(tok) != 0 && !(dw[5] & 1)) {
        nxt = dw[5];
    } else {
        nxt = dw[4];
    }
    if (nxt & 1) {
        return 0;
    }
    nxt &= ~0x1FU;
    if (!sim_addr_ok(nxt, 32)) {
        sim_viol(V_PTR_RANGE, "QH %#x next qTD %#x outside DMA memory", qa, nxt);
        dw[6] |= TOK_HALT;
        qh_write_overlay(qa, dw);
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        q[i] = m32(nxt + 4 * i);
    }
    if (!(q[2] & TOK_ACTIVE)) {
        return 0;
    }
    dw[3] = nxt;
    dw[4] = q[0];
    dw[5] = (q[1] & ~0x1EU) | (EPC_RL(dw[1]) << 1);
    if (dw[1] & EPC_DTC) {
        dw[6] = q[2];
    } else {
        dw[6] = (q[2] & ~TOK_DT) | (tok & TOK_DT);
    }
    dw[6] = (dw[6] & ~TOK_PING) | (tok & TOK_PING);
    dw[7] = q[3];
    dw[8] = q[4] & ~0xFFU;
    dw[9] = q[5] & ~0xFFFU;
    dw[10] = q[6];
    dw[11] = q[7];
    qh_write_overlay(qa, dw);
    s = seen_get(qa, 1);
    if (s) {
        s->qtd = nxt;
        s->qtd_token = q[2];
        s->qtd_next = q[0];
        s->qtd_owned = 1;
    }
    g_progress = 1;
    return 1;
}

static void seen_store(uint32_t qa, const uint32_t dw[12], int periodic)
{
    struct seen *s = seen_get(qa, 1);
    if (s) {
        memcpy(s->dw, dw, sizeof(s->dw));
        s->epoch = periodic ? g_ps_epoch : g_as_epoch;
    }
}

/* compare what the HC knows of a QH with memory, before it uses it */
static void qh_check_edits(uint32_t qa, const uint32_t mem[12], int held_unlinked, int periodic)
{
    struct seen *s = seen_get(qa, 0);

    if (!s || s->epoch != (periodic ? g_ps_epoch : g_as_epoch)) {
        return;
    }
    if (held_unlinked) {
        if (s->unlink_seq && g_answered_seq >= s->unlink_seq) {
            return; /* a doorbell rung after the unlink was answered: HC let it go */
        }
        if (memcmp(s->dw, mem, 3 * 4) != 0 || memcmp(&s->dw[3], &mem[3], 9 * 4) != 0) {
            sim_viol(V_QH_REUSE_CACHED,
                     "QH %#x was unlinked and modified while the HC still held it (no IAA answered "
                     "since the unlink, async schedule not stopped): hlp %#x->%#x epchar %#x->%#x "
                     "cur qTD %#x->%#x next %#x->%#x token %#x->%#x",
                     qa, s->dw[0], mem[0], s->dw[1], mem[1], s->dw[3], mem[3], s->dw[4], mem[4], s->dw[6],
                     mem[6]);
            memcpy(s->dw, mem, sizeof(s->dw)); /* report once per edit */
        }
        return;
    }
    if ((s->dw[6] & TOK_ACTIVE) && memcmp(&s->dw[3], &mem[3], 9 * 4) != 0) {
        sim_viol(V_OVERLAY_TOUCH,
                 "software modified the active overlay of linked QH %#x (cur qTD %#x->%#x, "
                 "next %#x->%#x, token %#x->%#x) with the %s schedule running",
                 qa, s->dw[3], mem[3], s->dw[4], mem[4], s->dw[6], mem[6],
                 periodic ? "periodic" : "asynchronous");
    }
    if ((s->dw[6] & TOK_ACTIVE) && (s->dw[1] != mem[1] || s->dw[2] != mem[2])) {
        sim_viol(V_OVERLAY_TOUCH, "endpoint characteristics of active linked QH %#x changed (%#x->%#x)",
                 qa, s->dw[1], mem[1]);
    }
}

/* one visit of an async QH; returns its horizontal link (0 on a fatal link problem) */
static uint32_t async_visit(uint32_t qa, uint32_t *budget, int *is_head, int reload_nak)
{
    uint32_t dw[12], mem[12];
    int from_cache = 0, unlinked;

    if (qh_read(qa, mem, "async walk") < 0) {
        return 0;
    }
    unlinked = !in_ring(qa); /* reached through a held QH: the HC still uses it */
    qh_check_edits(qa, mem, unlinked, 0);
    memcpy(dw, mem, sizeof(dw));
    if (g_sim_cfg.stale_cache && g_cache.valid && g_cache.addr == qa) {
        /* the HC executes from its copy: static fields and an active overlay */
        dw[1] = g_cache.dw[1];
        dw[2] = g_cache.dw[2];
        if (g_cache.dw[6] & TOK_ACTIVE) {
            memcpy(&dw[3], &g_cache.dw[3], 9 * 4);
        }
        from_cache = 1;
    }
    *is_head = (dw[1] & EPC_H) != 0;
    if (*is_head) {
        g_passed_head = 1;
    }
    if (reload_nak && EPC_RL(dw[1])) {
        dw[5] = (dw[5] & ~0x1EU) | (EPC_RL(dw[1]) << 1);
    }
    if (!(dw[6] & TOK_HALT) && *budget) {
        if (!(dw[6] & TOK_ACTIVE)) {
            qh_advance(qa, dw);
        }
        if ((dw[6] & TOK_ACTIVE) && !(dw[6] & TOK_HALT)) {
            int nak_blocked = EPC_RL(dw[1]) && ((dw[5] >> 1) & 0xF) == 0;
            if (!nak_blocked && qh_transact(qa, dw, 0)) {
                (*budget)--;
            }
        }
    }
    seen_store(qa, dw, 0);
    if (from_cache) {
        memcpy(&g_cache.dw[3], &dw[3], 9 * 4); /* the copy is what the HC keeps executing */
    }
    /* the horizontal link is read from memory when the HC leaves the QH */
    return m32(qa);
}

static void async_uframe(void)
{
    uint32_t budget = g_sim_cfg.async_budget, a = g_cur ? g_cur : R.aladdr;
    int visits = 0, heads = 0, since_head = 0, reload = 0;

    if (!a) {
        return;
    }
    g_progress = 0;
    while (visits < 400) {
        int is_head = 0;
        uint32_t l;

        if (sim_addr_ok(a, 48) && (m32(a + 4) & EPC_H)) {
            /* reclamation (4.8.3): a lap without a transaction ends this microframe's walk */
            if (heads > 0 && !g_progress) {
                break;
            }
            reload = heads == 0; /* Nak Cnt reload once per microframe, at the head */
            heads++;
            g_progress = 0;
            since_head = 0;
        } else if (++since_head > 128) {
            sim_viol(V_ASYNC_LINK, "async walk: 128 QHs without reaching an H-bit head (loop?)");
            g_cur = 0;
            R.usbcmd &= ~CMD_ASE;
            R.usbsts &= ~STS_AS;
            return;
        }
        if (!budget) {
            break;
        }
        l = async_visit(a, &budget, &is_head, reload);
        visits++;
        if (!budget) {
            break; /* parked on the QH it was serving: the next microframe resumes there */
        }
        if (heads > 1 && is_head) {
            /* counted above */
        }
        if (l == 0 || (l & 1) || ((l >> 1) & 3) != 1) {
            sim_viol(V_ASYNC_LINK, "async QH %#x horizontal link %#x is %s", a, l,
                     l == 0 ? "unreadable" : (l & 1) ? "terminated (T=1)" : "not a QH");
            g_cur = 0;
            return;
        }
        a = l & ~0x1FU;
    }
    g_cur = a;
    R.aladdr = g_cur;
    /* stale cache: keep the QH the walk stopped on */
    if (g_sim_cfg.stale_cache && g_cur && sim_addr_ok(g_cur, 48)) {
        if (!g_cache.valid || g_cache.addr != g_cur) {
            g_cache.valid = 1;
            g_cache.addr = g_cur;
            qh_read(g_cur, g_cache.dw, "cache fill");
        } else {
            /* refresh the overlay if idle, keep it while active */
            uint32_t mem[12];
            qh_read(g_cur, mem, "cache refresh");
            if (!(g_cache.dw[6] & TOK_ACTIVE)) {
                memcpy(&g_cache.dw[3], &mem[3], 9 * 4);
            }
            g_cache.dw[0] = mem[0];
        }
    }
    {
        uint32_t r[300];
        int n = ring_walk(r, 300, NULL);
        if (n > 0) {
            g_sim_stats.ring_len = (uint32_t)n;
            if ((uint32_t)n > g_sim_stats.ring_len_max) {
                g_sim_stats.ring_len_max = (uint32_t)n;
            }
            /* link events: QHs that were not in the ring at the previous microframe */
            static uint32_t prev[300];
            static int nprev;
            for (int i = 0; i < n && i < 300; i++) {
                int found = 0;
                for (int j = 0; j < nprev; j++) {
                    if (prev[j] == r[i]) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    g_sim_stats.qh_link_events++;
                    struct seen *s = seen_get(r[i], 1);
                    if (s) {
                        s->unlink_seq = 0;
                    }
                }
            }
            memcpy(prev, r, sizeof(uint32_t) * (size_t)(n < 300 ? n : 300));
            nprev = n < 300 ? n : 300;
        }
    }
}

/* the cached copy goes back to memory when the HC is done with it (models a write-back) */
static void cache_drop(void)
{
    g_cache.valid = 0;
}

/* ---------------- periodic ---------------- */
uint32_t sim_periodic_qhs(uint32_t *out, uint32_t max)
{
    uint32_t fl = fl_size(), n = 0;

    if (!R.plbase || !sim_addr_ok(R.plbase, fl * 4)) {
        return 0;
    }
    for (uint32_t f = 0; f < fl; f++) {
        uint32_t link = m32(R.plbase + f * 4);
        for (int guard = 0; !(link & 1) && guard < 64; guard++) {
            uint32_t a = link & ~0x1FU, k;
            if (!sim_addr_ok(a, 48)) {
                break;
            }
            if (((link >> 1) & 3) == 1) {
                for (k = 0; k < n && k < max; k++) {
                    if (out[k] == a) {
                        break;
                    }
                }
                if (k == n && n < max) {
                    out[n++] = a;
                }
            }
            link = m32(a);
        }
    }
    return n;
}

static void periodic_uframe(void)
{
    uint32_t fl = fl_size(), idx = (R.frindex >> 3) & (fl - 1), uf = R.frindex & 7;
    uint32_t link, visits = 0;
    uint32_t seenq[64];
    uint32_t nseen = 0;

    if (!sim_addr_ok(R.plbase, fl * 4)) {
        sim_viol(V_PTR_RANGE, "periodic frame list %#x outside DMA memory", R.plbase);
        R.usbcmd &= ~CMD_PSE;
        R.usbsts &= ~STS_PS;
        return;
    }
    link = m32(R.plbase + idx * 4);
    while (!(link & 1)) {
        uint32_t a = link & ~0x1FU, type = (link >> 1) & 3;
        if (++visits > 64) {
            sim_viol(V_PERIODIC_LINK, "periodic list of frame %u: more than 64 nodes (loop?)", idx);
            break;
        }
        if (!sim_addr_ok(a, 32) || (link & 0x18)) {
            sim_viol(V_PERIODIC_LINK, "periodic link %#x invalid (frame %u)", link, idx);
            break;
        }
        if (type != 1) {
            link = m32(a); /* iTD/siTD/FSTN: their first dword is the next link */
            continue;
        }
        {
            uint32_t dw[12];
            int dup = 0;
            if (qh_read(a, dw, "periodic walk") < 0) {
                break;
            }
            for (uint32_t i = 0; i < nseen; i++) {
                if (seenq[i] == a) {
                    dup = 1;
                }
            }
            if (dup) {
                sim_viol(V_PERIODIC_LINK, "QH %#x visited twice in frame %u", a, idx);
                break;
            }
            if (nseen < 64) {
                seenq[nseen++] = a;
            }
            qh_check_edits(a, dw, 0, 1);
            if ((dw[2] & 0xFF) == 0) {
                sim_viol(V_PERIODIC_LINK, "periodic QH %#x with S-mask 0", a);
            }
            if (((dw[2] >> 30) & 3) == 0) {
                sim_viol(V_PERIODIC_LINK, "periodic QH %#x with Mult 0", a);
            }
            if (dw[1] & EPC_H) {
                sim_viol(V_PERIODIC_LINK, "periodic QH %#x has the H bit", a);
            }
            if (EPC_RL(dw[1])) {
                sim_viol(V_PERIODIC_LINK, "periodic QH %#x with NAK reload %u (must be 0)", a, EPC_RL(dw[1]));
            }
            if ((dw[2] >> uf) & 1) {
                if (!(dw[6] & TOK_HALT)) {
                    if (!(dw[6] & TOK_ACTIVE)) {
                        qh_advance(a, dw);
                    }
                    if ((dw[6] & TOK_ACTIVE) && !(dw[6] & TOK_HALT)) {
                        qh_transact(a, dw, 1);
                        g_sim_stats.periodic_tx++;
                    }
                }
            }
            seen_store(a, dw, 1);
            link = dw[0];
        }
    }
}

/* ---------------- microframe ---------------- */
static void hc_uframe(void)
{
    uint32_t itc;

    g_sim_stats.uframes++;
    if ((R.usbcmd & CMD_RST) && g_now >= g_rst_done_ns) {
        R.usbcmd &= ~CMD_RST;
    }
    if (g_port_reset_done_ns && g_now >= g_port_reset_done_ns) {
        g_port_reset_done_ns = 0;
        R.portsc &= ~PS_PR;
        if (R.portsc & PS_CCS) {
            R.portsc |= PS_PE | PS_HSP | PS_PSPD_HS;
        }
    }
    if (g_halt_due_ns && g_now >= g_halt_due_ns) {
        g_halt_due_ns = 0;
        if (!(R.usbcmd & CMD_RS)) {
            R.usbsts |= STS_HCH;
            R.usbsts &= ~(STS_AS | STS_PS);
            g_cur = 0;
            cache_drop();
            g_as_epoch++;
            g_ps_epoch++;
        }
    }
    if (R.usbsts & STS_HCH) {
        return;
    }
    R.frindex = (R.frindex + 1) & 0x3FFF;
    if ((R.frindex & (fl_size() * 8 - 1)) == 0) {
        R.usbsts |= STS_FRI;
    }
    R.usbsts |= STS_SRI;
    /* schedule status follows the enables */
    if (((R.usbcmd & CMD_ASE) != 0) != ((R.usbsts & STS_AS) != 0) && g_now >= g_ase_due_ns && !g_sim_cfg.wedge) {
        R.usbsts ^= STS_AS;
        if (R.usbsts & STS_AS) {
            g_cur = R.aladdr;
        } else {
            g_cur = 0;
            cache_drop();
            g_as_epoch++;
        }
    }
    if (((R.usbcmd & CMD_PSE) != 0) != ((R.usbsts & STS_PS) != 0) && g_now >= g_pse_due_ns && !g_sim_cfg.wedge) {
        R.usbsts ^= STS_PS;
        if (!(R.usbsts & STS_PS)) {
            g_ps_epoch++;
        }
    }
    sim_dev_tick_uframe();
    if (R.usbsts & STS_PS) {
        periodic_uframe();
    }
    if (R.usbsts & STS_AS) {
        async_uframe();
    }
    /* doorbell */
    if ((R.usbcmd & CMD_IAAD) && g_now - g_ring_ns >= (uint64_t)g_sim_cfg.iaa_delay_uf * 125000) {
        int ok;
        if (R.usbsts & STS_AS) {
            ok = !g_sim_cfg.iaa_need_head || g_passed_head;
        } else {
            ok = g_sim_cfg.iaa_when_async_off;
        }
        if (ok) {
            R.usbcmd &= ~CMD_IAAD;
            R.usbsts |= STS_AAI;
            g_answered_seq = g_ring_seq;
            g_sim_stats.iaa_answers++;
            cache_drop();
        }
    }
    g_passed_head = 0;
    /* interrupt threshold */
    itc = (R.usbcmd >> 16) & 0xFF;
    if (itc == 0 || (R.frindex % itc) == 0) {
        if (g_pending_int & STS_UI) {
            g_sim_stats.usbint++;
        }
        if (g_pending_int & STS_UEI) {
            g_sim_stats.usberrint++;
        }
        R.usbsts |= g_pending_int;
        g_pending_int = 0;
    }
}

/* ---------------- lifecycle ---------------- */
void sim_init(void)
{
    static int done;
    if (done) {
        return;
    }
    done = 1;
    sim_mmio_init();
    arena_init();
    g_sim_cfg = k_cfg_default;
}

void sim_hc_power_on_reset(void)
{
    g_sim_cfg = k_cfg_default;
    {
        const char *e = getenv("SEED");
        sim_srand(e && *e ? (uint32_t)strtoul(e, NULL, 0) : g_sim_cfg.seed);
    }
    g_dev_present = 0;
    sim_dev_detach();
    reset_regs();
    memset(g_seen, 0, sizeof(g_seen));
    memset(&g_sim_stats, 0, sizeof(g_sim_stats));
    g_ring_seq = g_answered_seq = 0;
    g_head = 0;
    g_next_uf = g_now + 125000;
    g_gpt_last_us = g_now / 1000;
    sim_viol_clear();
    for (int i = 0; i < MAX_CBUF; i++) {
        if (g_cbuf[i].base) {
            free(g_cbuf[i].dirty);
            memset(&g_cbuf[i], 0, sizeof(g_cbuf[i]));
        }
    }
}
