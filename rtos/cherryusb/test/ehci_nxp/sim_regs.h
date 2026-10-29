/*
 * Force-included (-include) before the driver: its EHCI_NXP_REG_READ/EHCI_NXP_REG_WRITE go to
 * the register model. Any access that bypasses them still lands on the PROT_NONE page and is
 * emulated by the trap (sim_mmio.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SIM_REGS_H
#define SIM_REGS_H
#include <stdint.h>
uint32_t sim_drv_reg_read(uintptr_t addr);
void sim_drv_reg_write(uintptr_t addr, uint32_t val);
#define EHCI_NXP_REG_READ(addr)       sim_drv_reg_read((uintptr_t)(addr))
#define EHCI_NXP_REG_WRITE(addr, val) sim_drv_reg_write((uintptr_t)(addr), (uint32_t)(val))
#endif
