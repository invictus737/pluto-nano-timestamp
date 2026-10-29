# QSPI map and boot image limits (Pluto Nano, W25Q256, 32 MiB)

## Map

| QSPI | size | content | written by |
|---|---|---|---|
| 0x0000000 | 1 MiB | BOOT.bin: FSBL + U-Boot (Tezuka 2025.01); 0xFF000 = extra env (4 KiB) | DFU `boot.dfu` only |
| 0x0100000 | 128 KiB | U-Boot environment (`ENV_OFFSET`, `ENV_SIZE`) | RTEMS config store / fwupdate (with backup in /data) |
| 0x0120000 | 896 KiB | `/data`, JFFS2 ("nvmfs", `spare.dfu`) | RTEMS config store |
| 0x0200000 | 30 MiB (to 0x2000000) | FIT slot: `rtos.itb` / `rtos-nexus2.itb` | `fwupdate`, DFU `firmware.dfu` |

The FIT slot crosses the 16 MiB bank boundary. Inside it, the Linux firmware used
0x1000000 (1 MiB, read-only copy of BOOT.bin for a reset with EAR=1) and 0x1100000 (15 MiB UBI
`/mnt/data`); an RTEMS image above 14 MiB (end > 0x1000000) overwrites them. RTEMS does not use
either; restoring Linux rewrites the whole slot from the backup anyway.

`qspi_nor.c` refuses writes and erases outside 0x100000..0x1FFFFF, except the FIT slot while
`fwupdate` holds it open. U-Boot and BOOT.bin (0x0..0xFFFFF) are never writable from RTEMS.

## Reading beyond 16 MiB

- Board U-Boot = Tezuka v0.3.24 Nano build: Xilinx u-boot-xlnx a09d2660, config
  `board/tezuka/common/uboot.config` (nano_defconfig `BR2_TARGET_UBOOT_CUSTOM_CONFIG_FILE`) with
  `CONFIG_SPI_FLASH_BAR=y`, `CONFIG_ZYNQ_QSPI=y`, `CONFIG_SYS_BOOTM_LEN=0x3C00000`.
  `SPI_FLASH_SFDP_SUPPORT` depends on `!SPI_FLASH_BAR`, so the W25Q256 (`ef4019`, no 4B-opcode
  flag) runs 3-byte addressing with the bank (extended address) register.
- `spi-nor-core.c` in that tree: `sf probe` exits 4-byte mode and writes EAR=0 (Winbond case of
  `set_4byte`), `read_bar` uses 0xC8/0xC5 for non-Spansion parts; `spi_nor_read` splits a read
  at every 16 MiB boundary, calls `write_bar` per bank, and `clean_bar` sets EAR back to 0 after
  the read. So `sf read ${fit_load_address} 0x200000 ${fit_size}` reads up to the full 30 MiB.
- Boot command (env): `bootcmd=run nexus_qspiboot`,
  `nexus_qspiboot=if sf probe 0:0 50000000 0 && sf read ${fit_load_address} 0x200000 ${fit_size}
  && iminfo ${fit_load_address}; then run adi_loadvals; run nexus_pl_init; setenv bootargs ...;
  bootm ${fit_load_address}#${fit_config}; fi`. `iminfo` checks the md5 of the bitstream and
  of the kernel (the last image in the FIT, so the part above 16 MiB) before anything runs; a
  bad read stops at the U-Boot prompt, DFU stays available.
- RTEMS (`qspi_nor.c`) uses 4-byte-address opcodes (0x13 read, 0x12 program, 0xDC 64 KiB erase)
  and never changes ADS or EAR, so a reset from RTEMS always leaves bank 0 selected.
- Residual risk: a warm reset (button/watchdog) during the ~second U-Boot spends reading the
  upper bank leaves EAR=1; the BootROM then reads from 0x1000000 and finds no boot header.
  EAR is volatile: a power cycle recovers. No flash content is lost.

No U-Boot change and no new env variable is needed for images up to the slot size.

## RAM at boot (512 MiB DDR)

| address | what |
|---|---|
| 0x0104000 | RTEMS kernel load/entry (0x104040), gunzipped here by `bootm` |
| 0x2080000 | `fit_load_address`: the FIT read from QSPI (up to 0x3E80000 for 30 MiB) |
| 0xF000000 | bitstream copied out of the FIT (`fpga-1 load`) for the PL load |
| ~0x1E000000.. | relocated U-Boot, its 20 MiB malloc, stack, control DT |

Limits:

- FIT: <= 0x1E00000 bytes (30 MiB), the QSPI slot and the `dfu_ram` window.
- Kernel binary (uncompressed): must end below the FIT copy:
  0x2080000 - 0x104000 = 0x1F7C000 = 32 980 992 bytes (31.45 MiB). `CONFIG_SYS_BOOTM_LEN`
  (60 MiB) is not the binding limit. Today: 21.4 MB kernel, FIT ~11 MB.
- `rtos/ci/pack.sh` fails the build beyond either limit (`FIT_LOAD_ADDRESS` overrides 0x2080000).
- For a kernel above 31.45 MiB: `setenv fit_load_address 0x4000000; saveenv` (operator, U-Boot
  prompt). Kernel limit then 60 MiB (BOOTM_LEN), FIT copy ends at 0x5E00000, below the
  bitstream at 0xF000000. Build with `FIT_LOAD_ADDRESS=0x4000000`. Not needed today.

## fwupdate

`fwupdate [port] [reboot]`, then `nc <board> 4444 < rtos-nexus2.itb`.

- The FIT header (magic d00dfeed, totalsize <= 30 MiB) is checked on the first 64 bytes.
- The image is received into a buffer of its own size (+64 KiB tail) and written only when
  complete. If that allocation fails, it streams 64 KiB sectors to flash as they arrive (a
  broken connection then leaves a broken slot, fit_size unchanged, U-Boot's `iminfo` stops).
- Per 64 KiB sector: unchanged content is skipped (no erase, no program); otherwise erase,
  program, every page read back and compared.
- Then `fit_size` (hex, bytes received) and `fit_config=config-2` are written to the U-Boot env.

Speed: page program data goes through the TX FIFO in 32-byte bursts (only when the batched-read
self-test passed) and the page-program wait polls WIP for up to 5 ms instead of sleeping a
1 ms tick per page. The write is bound by the chip's page program time (under 1 ms typ., 3 ms
max per 256 B page) and 64 KiB erase (~150 ms typ.): estimated 3 to 4 min for a full 30 MiB
slot, much less when sectors did not change (not yet measured on the board). Quad page program (0x34) is not used: I/O3 is MIO5, which the boot jumper
grounds (see the JTAG recovery notes), and the controller only knows its own quad opcodes in
flash interface mode; the gain over the program time would be under 15 %.

## Serial fallback

YMODEM (`loady`) at 115200 8N1 moves about 11 KB/s: 11 MB FIT ~17 min, 25 MB ~40 min. `loady`
takes a baud rate (`loady ${fit_load_address} 460800`) but the Mac CH34x link garbled above
115200 before: try it only after a short `loady` test at the higher rate. The fast fallback is
USB DFU from U-Boot (`run dfu_ram` to boot an image from RAM, `run dfu_sf` + `dfu-util -a
firmware.dfu` to write the slot, then `setenv fit_size <hex>; saveenv`): seconds, not minutes.
