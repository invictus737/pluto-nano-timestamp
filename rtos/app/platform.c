/*
 * Pluto Nano platform overrides of weak RTEMS Zynq BSP hooks:
 * - MMU: the BSP maps only the PS peripherals; the PL AXI GP0/GP1 windows (AD9363 cores,
 *   DMACs) are added as device memory.
 * - Clocks: the FSBL decides them (the repo ships 495/666/950/1100 MHz CPU variants), so the
 *   A9 PERIPHCLK, cpu_1x and the UART reference are read back from the SLCR instead of being
 *   fixed at BSP build time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define ARM_CP15_TEXT_SECTION BSP_START_TEXT_SECTION

#include <bsp.h>
#include <bsp/start.h>
#include <bsp/arm-cp15-start.h>
#include <bsp/arm-a9mpcore-start.h>

#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define SLCR_ARM_PLL_CTRL 0xf8000100U
#define SLCR_DDR_PLL_CTRL 0xf8000104U
#define SLCR_IO_PLL_CTRL 0xf8000108U
#define SLCR_ARM_CLK_CTRL 0xf8000120U
#define SLCR_UART_CLK_CTRL 0xf8000154U
#define SLCR_CLK_621_TRUE 0xf80001c4U
#define PS_CLK_HZ 33333333ULL

static uint64_t pll_hz(uint32_t ctrl_addr)
{
  return PS_CLK_HZ * ((REG32(ctrl_addr) >> 12) & 0x7fU);
}

uint64_t nexus_arm_hz(void)
{
  uint32_t c = REG32(SLCR_ARM_CLK_CTRL);
  uint32_t div = (c >> 8) & 0x3fU;
  uint32_t src = (c >> 4) & 0x3U;
  uint64_t pll = src == 2 ? pll_hz(SLCR_DDR_PLL_CTRL)
               : src == 3 ? pll_hz(SLCR_IO_PLL_CTRL) : pll_hz(SLCR_ARM_PLL_CTRL);
  return div ? pll / div : 0;
}

uint64_t nexus_uart_ref_hz(void)
{
  uint32_t c = REG32(SLCR_UART_CLK_CTRL);
  uint32_t div = (c >> 8) & 0x3fU;
  uint32_t src = (c >> 4) & 0x3U;
  uint64_t pll = src == 2 ? pll_hz(SLCR_ARM_PLL_CTRL)
               : src == 3 ? pll_hz(SLCR_DDR_PLL_CTRL) : pll_hz(SLCR_IO_PLL_CTRL);
  return div ? pll / div : 0;
}

/* A plausible value or the BSP default: QEMU and odd FSBLs must not yield a zero clock. */
static uint32_t sane(uint64_t hz, uint64_t lo, uint64_t hi, uint32_t fallback)
{
  return (hz >= lo && hz <= hi) ? (uint32_t)hz : fallback;
}

uint32_t a9mpcore_clock_periphclk(void)
{
  return sane(nexus_arm_hz() / 2, 100000000ULL, 600000000ULL,
              BSP_ARM_A9MPCORE_PERIPHCLK);
}

uint32_t zynq_clock_cpu_1x(void)
{
  uint64_t div = (REG32(SLCR_CLK_621_TRUE) & 1U) ? 6 : 4;
  return sane(nexus_arm_hz() / div, 50000000ULL, 300000000ULL, ZYNQ_CLOCK_CPU_1X);
}

uint32_t zynq_uart_input_clock(void)
{
  return sane(nexus_uart_ref_hz(), 10000000ULL, 200000000ULL, ZYNQ_CLOCK_UART);
}

/*
 * The BSP default sections, except that the non-cacheable DMA pool is Normal non-cacheable
 * instead of Device memory: libbsd's USB code memcpy()s SCSI commands into DMA frame buffers at
 * odd offsets, and an unaligned access to Device memory is a data abort (seen on the board with
 * the AIC8800 storage-mode eject). Normal non-cacheable stays coherent for DMA.
 */
#define NEXUS_SECTION(b, e, f) {.begin = (uint32_t)(b), .end = (uint32_t)(e), .flags = (f)}
BSP_START_DATA_SECTION static const arm_cp15_start_section_config
nexus_mmu_config_table[] = {
  NEXUS_SECTION(bsp_section_fast_text_begin, bsp_section_fast_text_end, ARMV7_MMU_CODE_CACHED),
  NEXUS_SECTION(bsp_section_fast_data_begin, bsp_section_fast_data_end, ARMV7_MMU_DATA_READ_WRITE_CACHED),
  NEXUS_SECTION(bsp_section_start_begin, bsp_section_start_end, ARMV7_MMU_CODE_CACHED),
  NEXUS_SECTION(bsp_section_vector_begin, bsp_section_vector_end, ARMV7_MMU_DATA_READ_WRITE_CACHED),
  NEXUS_SECTION(bsp_section_text_begin, bsp_section_text_end, ARMV7_MMU_CODE_CACHED),
  NEXUS_SECTION(bsp_section_rodata_begin, bsp_section_rodata_end, ARMV7_MMU_DATA_READ_ONLY_CACHED),
  NEXUS_SECTION(bsp_section_data_begin, bsp_section_data_end, ARMV7_MMU_DATA_READ_WRITE_CACHED),
  NEXUS_SECTION(bsp_section_bss_begin, bsp_section_bss_end, ARMV7_MMU_DATA_READ_WRITE_CACHED),
  NEXUS_SECTION(bsp_section_rtemsstack_begin, bsp_section_rtemsstack_end, ARMV7_MMU_DATA_READ_WRITE_CACHED),
  NEXUS_SECTION(bsp_section_noinit_begin, bsp_section_noinit_end, ARMV7_MMU_DATA_READ_WRITE_CACHED),
  NEXUS_SECTION(bsp_section_work_begin, bsp_section_work_end, ARMV7_MMU_DATA_READ_WRITE_CACHED),
  NEXUS_SECTION(bsp_section_stack_begin, bsp_section_stack_end, ARMV7_MMU_DATA_READ_WRITE_CACHED),
  NEXUS_SECTION(bsp_section_nocache_begin, bsp_section_nocache_end, ARMV7_MMU_DATA_READ_WRITE),
  NEXUS_SECTION(bsp_section_nocachenoload_begin, bsp_section_nocachenoload_end, ARMV7_MMU_DATA_READ_WRITE),
  NEXUS_SECTION(bsp_translation_table_base, bsp_translation_table_end, ARMV7_MMU_DATA_READ_WRITE_CACHED),
#if defined(RTEMS_SMP)
  {.begin = 0xffff0000U, .end = 0xffffffffU, .flags = ARMV7_MMU_DEVICE},
#endif
  /* PL: M_AXI_GP0 0x40000000..0x7fffffff, M_AXI_GP1 0x80000000..0xbfffffff */
  {.begin = 0x40000000U, .end = 0xc0000000U, .flags = ARMV7_MMU_DEVICE},
  {.begin = 0xe0000000U, .end = 0xe0200000U, .flags = ARMV7_MMU_DEVICE},
  {.begin = 0xf8000000U, .end = 0xf9000000U, .flags = ARMV7_MMU_DEVICE}
};

BSP_START_TEXT_SECTION void zynq_setup_mmu_and_cache(void)
{
  uint32_t ctrl = arm_cp15_start_setup_mmu_and_cache(
    ARM_CP15_CTRL_A,
    ARM_CP15_CTRL_AFE | ARM_CP15_CTRL_Z
  );

  arm_cp15_start_setup_translation_table_and_enable_mmu_and_cache(
    ctrl,
    (uint32_t *) bsp_translation_table_base,
    ARM_MMU_DEFAULT_CLIENT_DOMAIN,
    &nexus_mmu_config_table[0],
    RTEMS_ARRAY_SIZE(nexus_mmu_config_table)
  );
}

/*
 * ARM EABI thread pointer helper for code that does not read TPIDRURO inline (Rust std on
 * armv7-rtems-eabihf uses ELF TLS through this call). RTEMS on ARMv7 keeps the thread pointer
 * in TPIDRURO; the helper may clobber only r0.
 */
__asm__(
  ".pushsection .text.__aeabi_read_tp, \"ax\", %progbits\n"
  ".syntax unified\n"
  ".thumb\n"
  ".align 2\n"
  ".global __aeabi_read_tp\n"
  ".type __aeabi_read_tp, %function\n"
  ".thumb_func\n"
  "__aeabi_read_tp:\n"
  "  mrc p15, 0, r0, c13, c0, 3\n"
  "  bx lr\n"
  ".size __aeabi_read_tp, . - __aeabi_read_tp\n"
  ".popsection\n"
);

/*
 * On any RTEMS termination (shell shutdown, fatal error) hold the ULPI PHY in reset and release
 * it: the PHY otherwise keeps the host/VBUS state RTEMS gave it, and U-Boot's DFU gadget is not
 * seen by the computer after the following soft reset.
 */
#define GPIO_MASK_DATA_1_LSW 0xe000a008U
#define GPIO_DIRM_1 0xe000a244U
#define GPIO_OEN_1 0xe000a248U
#define USB_PHY_RESET_BIT (1U << 14) /* MIO 46 */

void nexus_fatal_extension(rtems_fatal_source source, bool always_set_to_false, rtems_fatal_code code)
{
  (void)source;
  (void)always_set_to_false;
  (void)code;
  REG32(GPIO_DIRM_1) |= USB_PHY_RESET_BIT;
  REG32(GPIO_OEN_1) |= USB_PHY_RESET_BIT;
  REG32(GPIO_MASK_DATA_1_LSW) = ((~USB_PHY_RESET_BIT & 0xffffU) << 16);
  for (volatile uint32_t i = 0; i < 200000; ++i) {
  }
  REG32(GPIO_MASK_DATA_1_LSW) = ((~USB_PHY_RESET_BIT & 0xffffU) << 16) | USB_PHY_RESET_BIT;
}
