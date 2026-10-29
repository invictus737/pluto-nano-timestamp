/*
 * EHCI 1.0 host controller simulator (ChipIdea/Zynq-7000 USB0 flavour) for host tests of the
 * CherryUSB ehci_nxp port. Self-contained: no CherryUSB header is included here, so the model
 * and the driver never share macros.
 *
 * The register block is a PROT_NONE page; every driver load/store traps (SIGSEGV) and is
 * emulated (sim_mmio.c), so write-1-to-clear, the doorbell, self-clearing bits and status
 * that lags the command register behave as on silicon, and a busy-wait on a register
 * advances simulated time. Descriptors hold 32-bit addresses: every DMA-visible object must
 * sit below 4 GiB (non-PIE binary, low arena from sim_dma_alloc()); the model checks every
 * pointer it follows against those ranges.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SIM_HC_H
#define SIM_HC_H

#include <stddef.h>
#include <stdint.h>

/* virtual address of the controller block (reg_base of the bus = this, USBCMD at +0x140) */
#define SIM_REG_BASE 0x10000000UL
#define SIM_HCCR_OFFSET 0x100U

/* ---- configuration (may be changed between tests) ---- */
struct sim_cfg {
    uint32_t mmio_cost_ns;     /* simulated time per register access (busy-wait progress) */
    uint32_t as_delay_uf;      /* microframes until USBSTS.AS/PS follow USBCMD.ASE/PSE */
    uint32_t as_lag_max_uf;    /* "AS lags" mode: extra random delay 0..max microframes */
    uint32_t iaa_delay_uf;     /* doorbell answered no sooner than this after the ring */
    int iaa_need_head;         /* answer the doorbell only when the HC passes the H-bit head */
    int iaa_when_async_off;    /* 1: a doorbell rung with AS=0 is answered, 0: never (strict) */
    int stale_cache;           /* HC keeps a cached copy of the QH it is parked on */
    uint32_t async_budget;     /* async packets per microframe */
    uint32_t irq_latency_uf;   /* interrupt line raised -> handler runs */
    int preempt;               /* run the interrupt handler when the driver leaves its
                                  outermost critical section (models the IRQ thread) */
    uint32_t port_reset_ms;    /* ChipIdea: PORTSC.PR self-clears after this */
    unsigned seed;
    int wedge;                 /* USBSTS.AS/PS stop following ASE/PSE until HCRESET (wedged core) */
};
extern struct sim_cfg g_sim_cfg;

/* ---- violations: spec rules the driver broke (each counted, first few printed) ---- */
enum sim_viol {
    V_PTR_RANGE,        /* descriptor/buffer pointer outside DMA-visible memory */
    V_PTR_ALIGN,        /* QH/qTD/frame list not aligned as EHCI requires */
    V_ASYNC_LINK,       /* async HLP terminates, not a QH, or the ring has no/2 H heads */
    V_ASE_HANDSHAKE,    /* ASE/PSE changed while USBSTS.AS/PS did not yet follow */
    V_ASYNCADDR_LIVE,   /* ASYNCLISTADDR written with the async schedule on */
    V_PERIODICBASE_LIVE,/* PERIODICLISTBASE changed with the periodic schedule running */
    V_QH_REUSE_CACHED,  /* QH the HC may still hold (no IAA/AS-off since unlink) modified */
    V_QTD_REUSE_HELD,   /* qTD modified/reused while the HC held it in an overlay */
    V_OVERLAY_TOUCH,    /* software modified the overlay of an active QH in the schedule */
    V_TOGGLE,           /* data toggle mismatch (device discarded a packet / data lost) */
    V_MPS,              /* QH max packet differs from the endpoint */
    V_ADDR,             /* transaction to an address/endpoint nobody answers */
    V_EPS,              /* wrong endpoint speed in the QH */
    V_DMA_DIRTY,        /* DMA read of an uncleaned / DMA write over a dirty cache line */
    V_IAA_ASYNC_OFF,    /* doorbell rung with the async schedule off */
    V_RS_MODE,          /* RS set without USBMODE.CM=host, or HCRESET while running */
    V_CPAGE,            /* transfer ran past buffer page 4 */
    V_PERIODIC_LINK,    /* periodic list malformed (loop, bad type, Mult=0, no S-mask) */
    V_SLEEP_IN_CRIT,    /* driver slept/waited while holding its critical section */
    V_QTD_ACTIVE_HALT,  /* qTD left Active on a halted queue then reported done */
    V_MISC,
    V_COUNT
};
extern uint32_t g_viol[V_COUNT];
extern const char *const g_viol_name[V_COUNT];
void sim_viol(enum sim_viol v, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
uint32_t sim_viol_total(void);
void sim_viol_clear(void);
void sim_viol_dump(void);

/* ---- lifecycle / time ---- */
void sim_init(void);                 /* maps the register page and the DMA arena (once) */
void sim_hc_power_on_reset(void);    /* register reset values, devices detached */
uint64_t sim_now_ns(void);
uint32_t sim_now_us(void);
/* advance simulated time; the interrupt handler is run when allowed (not inside the driver) */
void sim_advance_ns(uint64_t ns);
void sim_advance_us(uint32_t us);
void sim_advance_ms(uint32_t ms);
/* advance until cond(arg) or timeout; returns 1 if cond became true */
int sim_run_until(int (*cond)(void *), void *arg, uint32_t timeout_ms);

/* the test provides it: called when the line is up and a handler may run */
void sim_irq_handler(void);
/* in-driver depth bookkeeping (OSAL stubs): no handler while > 0 */
extern int g_sim_crit, g_sim_in_isr, g_sim_in_mmio;
void sim_preempt_point(void);        /* the IRQ thread may run here (if not in crit/ISR) */

/* ---- register model (used by sim_mmio.c) ---- */
uint32_t sim_reg_read(uint32_t off, unsigned size);
void sim_reg_write(uint32_t off, uint32_t val, unsigned size);
uint32_t sim_reg_peek(uint32_t off); /* no side effects, no time */

/* ---- memory ---- */
void *sim_dma_alloc(size_t size, size_t align);
void sim_dma_free(void *p);
size_t sim_dma_in_use(void);
uint32_t sim_dma_allocs(void);
int sim_addr_ok(uint32_t addr, uint32_t len);
/* CPU cache model of the test's data buffers (other memory is treated as non-cacheable) */
void sim_cache_track(void *buf, size_t len, int is_in); /* register a cacheable buffer */
void sim_cache_untrack(void *buf);
void sim_cache_cpu_wrote(void *buf, size_t len); /* lines become dirty */
void sim_cache_clean(uintptr_t addr, size_t len);
void sim_cache_invalidate(uintptr_t addr, size_t len);

/* ---- statistics ---- */
struct sim_stats {
    uint64_t uframes, packets, naks, iaa_rings, iaa_answers, ase_toggles, pse_toggles;
    uint64_t qh_link_events;  /* a QH address entering the async ring */
    uint64_t usbint, usberrint, irq_calls;
    uint64_t stalls, xacterrs, halts, shorts, babbles;
    uint64_t periodic_tx;
    uint32_t ring_len, ring_len_max, periodic_qhs;
};
extern struct sim_stats g_sim_stats;
uint32_t sim_async_ring(uint32_t *out, uint32_t max); /* QHs linked in the ring (memory view) */
uint32_t sim_rand(void);
void sim_srand(uint32_t s);
uint32_t sim_periodic_qhs(uint32_t *out, uint32_t max);
uint32_t sim_hc_current_qh(void); /* QH the async engine is parked on */

/* ---- device side (sim_dev.c) ---- */
enum sim_hs { HS_ACK, HS_NAK, HS_STALL, HS_NORESP, HS_BABBLE };
/* one bus transaction. pid: 0 OUT, 1 IN, 2 SETUP. For IN, *len is in: max, out: sent */
enum sim_hs sim_dev_transact(uint8_t addr, uint8_t ep, uint8_t pid, int toggle, uint8_t *buf,
                             uint32_t *len, uint16_t mps, int is_periodic);
void sim_dev_tick_uframe(void);
int sim_dev_present(void);

/* fault injection: host system error (USBSTS.SEI, controller halts itself, RS cleared) */
void sim_hc_inject_hse(void);

/* port model */
void sim_port_connect(void);
void sim_port_disconnect(void);

#endif
