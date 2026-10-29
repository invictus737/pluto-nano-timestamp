/*
 * Register access trap: the controller block is a PROT_NONE page at SIM_REG_BASE. Every load
 * or store the driver makes there raises SIGSEGV; the handler decodes the AArch64 load/store,
 * performs it on the register model (sim_reg_read/write) and steps over the instruction.
 * Only general-purpose register loads/stores are expected from volatile accessors: LDR/STR
 * (unsigned offset, unscaled, pre/post-index, register offset), any size, with sign/zero
 * extension. Anything else aborts with the instruction word.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "sim_hc.h"

#if !defined(__aarch64__)
#error "the register trap decodes AArch64 loads/stores: build in an arm64 Linux (colima) container"
#endif

static uint64_t *xreg(ucontext_t *uc, unsigned r)
{
    return r == 31 ? (uint64_t *)&uc->uc_mcontext.sp : (uint64_t *)&uc->uc_mcontext.regs[r];
}

static void die(const char *what, uint32_t insn, uintptr_t pc, uintptr_t addr)
{
    char b[160];
    int n = snprintf(b, sizeof(b), "sim_mmio: %s insn %08x pc %lx addr %lx\n", what, insn,
                     (unsigned long)pc, (unsigned long)addr);
    (void)!write(2, b, (size_t)n);
    _exit(3);
}

static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    uintptr_t a = (uintptr_t)si->si_addr;
    uintptr_t pc = uc->uc_mcontext.pc;
    uint32_t insn = *(const uint32_t *)pc;
    unsigned size, opc, rt, rn, wb = 0;
    int64_t wb_off = 0;

    (void)sig;
    if (a < SIM_REG_BASE || a >= SIM_REG_BASE + 4096) {
        char b[128];
        int n = snprintf(b, sizeof(b), "SIGSEGV at %lx (pc %lx): not a register access\n",
                         (unsigned long)a, (unsigned long)pc);
        (void)!write(2, b, (size_t)n);
        signal(SIGSEGV, SIG_DFL);
        return; /* re-faults with the default action: core/abort */
    }
    size = insn >> 30;
    opc = (insn >> 22) & 3;
    rt = insn & 31;
    rn = (insn >> 5) & 31;
    if ((insn & 0x3F000000U) == 0x39000000U) {
        /* unsigned immediate offset */
    } else if ((insn & 0x3F200000U) == 0x38000000U) {
        unsigned idx = (insn >> 10) & 3;
        int64_t imm9 = (int64_t)((int32_t)(((insn >> 12) & 0x1FF) << 23) >> 23);
        if (idx == 1 || idx == 3) {
            wb = 1;
            wb_off = imm9;
        }
    } else if ((insn & 0x3F200C00U) == 0x38200800U) {
        /* register offset */
    } else {
        die("unsupported register access", insn, pc, a);
        return;
    }
    if (size == 3 && opc >= 2) {
        die("prefetch/unallocated on register page", insn, pc, a);
    }
    {
        unsigned bytes = 1U << size;
        uint32_t off = (uint32_t)(a - SIM_REG_BASE);

        g_sim_in_mmio++;
        if (opc == 0) {
            uint64_t v = rt == 31 ? 0 : uc->uc_mcontext.regs[rt];
            if (bytes == 8) {
                sim_reg_write(off, (uint32_t)v, 4);
                sim_reg_write(off + 4, (uint32_t)(v >> 32), 4);
            } else {
                sim_reg_write(off, (uint32_t)v, bytes);
            }
        } else {
            uint64_t v;
            if (bytes == 8) {
                v = sim_reg_read(off, 4) | ((uint64_t)sim_reg_read(off + 4, 4) << 32);
            } else {
                v = sim_reg_read(off, bytes);
            }
            if (opc >= 2) { /* sign extend */
                unsigned bits = bytes * 8;
                int64_t s = (int64_t)(v << (64 - bits)) >> (64 - bits);
                v = opc == 3 ? (uint64_t)(uint32_t)s : (uint64_t)s;
            }
            if (rt != 31) {
                uc->uc_mcontext.regs[rt] = v;
            }
        }
        g_sim_in_mmio--;
    }
    if (wb) {
        *xreg(uc, rn) += (uint64_t)wb_off;
    }
    uc->uc_mcontext.pc = pc + 4;
}

void sim_mmio_init(void)
{
    struct sigaction sa;
    void *p = mmap((void *)SIM_REG_BASE, 4096, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    if (p != (void *)SIM_REG_BASE) {
        perror("sim_mmio: register page");
        exit(2);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}
