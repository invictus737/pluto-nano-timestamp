/*
 * QA build: force-included before the driver instead of sim_regs.h. Same routing to the
 * register model, plus a count of every driver register access (CPU-overhead measurement).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QA_REGS_H
#define QA_REGS_H
#include <stdint.h>
uint32_t sim_drv_reg_read(uintptr_t addr);
void sim_drv_reg_write(uintptr_t addr, uint32_t val);
extern uint64_t g_qa_reg_rd, g_qa_reg_wr;
#define EHCI_NXP_REG_READ(addr)       (g_qa_reg_rd++, sim_drv_reg_read((uintptr_t)(addr)))
#define EHCI_NXP_REG_WRITE(addr, val) (g_qa_reg_wr++, sim_drv_reg_write((uintptr_t)(addr), (uint32_t)(val)))
#endif
