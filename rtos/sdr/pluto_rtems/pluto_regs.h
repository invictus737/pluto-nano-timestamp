/*
 * Pluto Nano (xc7z010 + AD9363A, ADI pluto HDL v0.39 + timestamping) addresses, from
 * board/nano/dts/nano.dtsi and the HDL patch firmware/patches/v0.39/hdl/0001.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PLUTO_REGS_H
#define PLUTO_REGS_H

/* axi_ad9361 (one AXI slave): ADC common + channels at +0x0000, DAC at +0x4000 */
#define PLUTO_ADC_BASE 0x79020000u
#define PLUTO_DAC_BASE 0x79024000u
#define PLUTO_REG_GPIO_IN 0x00b8u  /* up_*_gpio_in  (word 0x2e) */
#define PLUTO_REG_GPIO_OUT 0x00bcu /* up_*_gpio_out (word 0x2f) */
#define PLUTO_REG_ADC_CHAN_CNTRL(c) (0x0400u + (c) * 0x40u)
#define PLUTO_REG_ADC_CHAN_STATUS(c) (0x0404u + (c) * 0x40u)
#define PLUTO_REG_DAC_CHAN_CNTRL_7(c) (0x0418u + (c) * 0x40u) /* DAC data select */
#define PLUTO_REG_DAC_SYNC 0x0044u
#define PLUTO_REG_DMA_STATUS 0x0088u /* ADI_REG_DMA_STATUS: [2] overflow, [1] underflow */
#define PLUTO_DAC_SEL_DMA 2u
#define PLUTO_DAC_SEL_ZERO 3u
/* RX: up_adc_gpio_out[31:1] = timestamp_every (bus words), [0] = x8 decimator enable.
 * TX: up_dac_gpio_out[31:1] = timestamp_every (bus words), [0] = x8 interpolator enable;
 *     up_dac_gpio_in = upack timestamp discarded_block_count (late / too early blocks). */

/* AXI DMAC (ADI axi_dmac) */
#define PLUTO_RX_DMAC_BASE 0x7c400000u
#define PLUTO_TX_DMAC_BASE 0x7c420000u
/* GIC: DT "interrupts = <0 57 4>" / "<0 56 4>" -> SPI 57/56 -> interrupt ID + 32 */
#define PLUTO_RX_DMAC_IRQ (57u + 32u)
#define PLUTO_TX_DMAC_IRQ (56u + 32u)

#define DMAC_REG_VERSION 0x000u
#define DMAC_REG_PERIPH_ID 0x004u
#define DMAC_REG_SCRATCH 0x008u
#define DMAC_REG_IDENT 0x00cu      /* 'DMAC' on >= 4.3 */
#define DMAC_REG_INTF_DESC 0x010u
#define DMAC_REG_IRQ_MASK 0x080u   /* 1 = masked */
#define DMAC_REG_IRQ_PENDING 0x084u /* write 1 to clear */
#define DMAC_REG_IRQ_SOURCE 0x088u
#define DMAC_REG_CTRL 0x400u
#define DMAC_REG_TRANSFER_ID 0x404u
#define DMAC_REG_TRANSFER_SUBMIT 0x408u
#define DMAC_REG_FLAGS 0x40cu
#define DMAC_REG_DEST_ADDRESS 0x410u
#define DMAC_REG_SRC_ADDRESS 0x414u
#define DMAC_REG_X_LENGTH 0x418u
#define DMAC_REG_Y_LENGTH 0x41cu
#define DMAC_REG_DEST_STRIDE 0x420u
#define DMAC_REG_SRC_STRIDE 0x424u
#define DMAC_REG_TRANSFER_DONE 0x428u
#define DMAC_REG_ACTIVE_TRANSFER_ID 0x42cu
#define DMAC_REG_STATUS 0x430u
#define DMAC_CTRL_ENABLE 0x1u
#define DMAC_IRQ_SOT 0x1u
#define DMAC_IRQ_EOT 0x2u
#define DMAC_FLAG_CYCLIC 0x1u
#define DMAC_FLAG_LAST 0x2u
#define DMAC_IDENT_MAGIC 0x444d4143u

/* AD9363 control */
#define PLUTO_SPI_BASE 0xe0006000u /* PS SPI0 (EMIO), CS0, mode 1, <= 10 MHz */
#define PLUTO_SPI_CS 0
#define PLUTO_SPI_MAX_HZ 10000000u
#define PLUTO_GPIO_BASE 0xe000a000u
/* gpio0 numbers as in the DT (MIO 0..53, EMIO 54..117) */
#define PLUTO_GPIO_EN_AGC 66
#define PLUTO_GPIO_RESETB 67
#define PLUTO_GPIO_BAND_CTL0 81
#define PLUTO_GPIO_BAND_CTL1 82
#define PLUTO_GPIO_BAND_CTL2 83

/* SLCR */
#define SLCR_BASE 0xf8000000u
#define SLCR_LOCK 0xf8000004u
#define SLCR_UNLOCK 0xf8000008u
#define SLCR_LOCK_KEY 0x767bu
#define SLCR_UNLOCK_KEY 0xdf0du
#define SLCR_ARM_PLL_CTRL 0xf8000100u
#define SLCR_DDR_PLL_CTRL 0xf8000104u
#define SLCR_IO_PLL_CTRL 0xf8000108u
#define SLCR_SPI_CLK_CTRL 0xf8000158u
#define SLCR_SPI_RST_CTRL 0xf800021cu
#define SLCR_FPGA_RST_CTRL 0xf8000240u
#define SLCR_LVL_SHFTR_EN 0xf8000900u
#define PS_CLK_HZ 33333333ull

/* PS XADC interface (devcfg) for the Zynq die temperature */
#define XADCIF_CFG 0xf8007100u
#define XADCIF_INT_STS 0xf8007104u
#define XADCIF_MSTS 0xf800710cu
#define XADCIF_CMDFIFO 0xf8007110u
#define XADCIF_RDFIFO 0xf8007114u
#define XADCIF_MCTL 0xf8007118u

#endif
