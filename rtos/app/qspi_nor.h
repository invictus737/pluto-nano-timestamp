/*
 * Pluto Nano QSPI NOR (W25Q256, 32 MiB) access for the configuration store. Uses the RTEMS
 * xilinx-zynq zqspi transfer layer with absolute 4-byte-address opcodes (0x13 read, 0x12 page
 * program, 0xDC 64 KiB erase): the chip's 3-byte/4-byte mode and extended address register are
 * never changed, so U-Boot and the BootROM find the flash as they left it.
 *
 * Writes and erases are confined to QSPI_NOR_WRITE_LO..QSPI_NOR_WRITE_HI (U-Boot env + nvmfs),
 * plus the firmware slot while fwupdate holds it open; every program and erase is verified by
 * reading back.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEXUS_QSPI_NOR_H
#define NEXUS_QSPI_NOR_H

#include <stddef.h>
#include <stdint.h>

#define QSPI_NOR_SIZE 0x2000000u     /* 32 MiB; every offset is sent as a 4-byte address */
#define QSPI_NOR_SECTOR 0x10000u     /* 64 KiB erase block */
#define QSPI_NOR_ENV_OFF 0x100000u   /* U-Boot environment, 128 KiB */
#define QSPI_NOR_ENV_SIZE 0x20000u
#define QSPI_NOR_DATA_OFF 0x120000u  /* "nvmfs": JFFS2 /data */
#define QSPI_NOR_DATA_SIZE 0xE0000u
#define QSPI_NOR_WRITE_LO QSPI_NOR_ENV_OFF
#define QSPI_NOR_WRITE_HI (QSPI_NOR_DATA_OFF + QSPI_NOR_DATA_SIZE) /* 0x200000, exclusive */

int qspi_nor_init(void);  /* 0 when a W25Q256-class (JEDEC EF xx 19) flash answered */
uint32_t qspi_nor_jedec(void);
int qspi_nor_read(uint32_t addr, void *buf, size_t len);
int qspi_nor_write(uint32_t addr, const void *buf, size_t len); /* erased area only */
int qspi_nor_erase_sector(uint32_t addr);                     /* 64 KiB aligned */

/* The boot image area (FIT at 0x200000, up to the end of the 32 MiB chip, 30 MiB) is writable
 * only between qspi_nor_fw_window(1) and qspi_nor_fw_window(0): the network firmware update.
 * It crosses the 16 MiB bank boundary: an image above 14 MiB (end > 0x1000000) overwrites the
 * Linux firmware's upper-bank boot copy (0x1000000) and its UBI data (0x1100000..), which the
 * RTEMS image does not use. U-Boot (Tezuka, CONFIG_SPI_FLASH_BAR) reads across the boundary by
 * switching the extended address register per 16 MiB bank and clears it after the read. See
 * rtos/QSPI.md. */
#define QSPI_NOR_FW_OFF 0x200000u
#define QSPI_NOR_FW_END QSPI_NOR_SIZE
#define QSPI_NOR_LOWER_BANK_END 0x1000000u
void qspi_nor_fw_window(int open);

_Static_assert(QSPI_NOR_FW_OFF >= QSPI_NOR_WRITE_HI, "firmware slot overlaps U-Boot env or /data");
_Static_assert(QSPI_NOR_FW_OFF % QSPI_NOR_SECTOR == 0, "firmware slot not sector aligned");

#endif
