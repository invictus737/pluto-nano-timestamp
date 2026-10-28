# AIC8800 (D80 + DC) on RTEMS 6.2 / rtems-libbsd: porting specification

Target: milestone M2c (Wi-Fi) and M2d (Bluetooth) of `rtos/PLAN.md`. This document is the
interface specification for a new driver; it is derived from reading the Linux driver and
the libbsd sources, and every statement cites where it comes from.

## 0. Sources and citation keys

| Key | Path |
|-----|------|
| `LF/` | Linux loader `drivers/aic8800/aic_load_fw/` of github.com/shenmintao/aic8800d80 @ `a9683a4` (branch `legacy-mcu1`) |
| `FD/` | Linux Wi-Fi driver `drivers/aic8800/aic8800_fdrv/` (same commit) |
| `FW/` | firmware directory `fw/` (same commit) |
| `RD/` | that repository's `README.md`, `tests/` |
| `PR/` | this repository (`/Users/ctermure/Work/pluto-rtos/`) |
| `LB/` | `rtems-libbsd-fb14-6.2/freebsd/` (FreeBSD 14 USB, net80211, wpa_supplicant) |

Compile-time configuration that applies to everything below (`FD/Makefile`): `CONFIG_RWNX_FULLMAC`,
`AICWF_USB_SUPPORT`, `CONFIG_USB_MSG_OUT_EP=y`, `CONFIG_USB_MSG_IN_EP=y`, `CONFIG_USB_RX_AGGR=n`,
`CONFIG_USB_TX_AGGR=n`, `CONFIG_USB_RX_REASSEMBLE=n`, `CONFIG_USE_USB_ZERO_PACKET=y`,
`CONFIG_RX_REORDER=y` (-> `AICWF_RX_REORDER`), `CONFIG_COEX=y`, `CONFIG_ROM_PATCH_EN=y`,
`CONFIG_DPD=y`, `CONFIG_FORCE_DPD_CALIB=y`, `CONFIG_USB_BT=y`, `CONFIG_RWNX_MUMIMO_TX=n`,
`CONFIG_RWNX_SPLIT_TX_BUF=n`. Loader: `CONFIG_USB_MSG_EP=y`, `CONFIG_LOAD_BT_CONF=n` (`LF/Makefile`).
Our in-tree build adds `CONFIG_USE_FW_REQUEST=y` (request_firmware) and `PLATFORM_UBUNTU=n`
(`PR/firmware/patches/aic8800/0001-...patch`, Makefile hunks).

### 0.1 Licence constraint (read first)

The Linux driver is GPL-2.0-only (`RD/README.md` "License"; `FD/rwnx_main.c` `MODULE_LICENSE("GPL")`,
RivieraWaves/AICSemi copyright headers). rtems-libbsd is BSD and the RTEMS image is one static
binary. Translating `FD/` code into the RTEMS driver makes the image a GPL-2.0 derivative.
Recommendation: write the RTEMS driver fresh (BSD-2-Clause, like `PR/rtos/libbsd/aic_modeswitch.c`)
from this specification (message IDs, struct layouts, register addresses and sequences are
interface facts), modelled on `LB/sys/dev/usb/wlan/if_rsu.c`, without copying `FD/` source.
Grey zone needing an explicit owner decision: the DC calibration data tables embedded in
`FD/aicwf_compat_8800dc.c` (`ldpc_cfg_ram[]` l.271, `agc_cfg_ram[]` l.948, `txgain_map*[96]` l.1464ff,
`syscfg_tbl_8800dc`, `patch_tbl_wifisetting`); option: extract them at build time into a data blob
loaded like firmware, kept outside the BSD source tree.

Firmware blobs (`FW/aic8800D80`, `FW/aic8800DC`) carry no licence text in the repository; the D80 set
is the Radxa SDK V3 set (`RD/tests/issue58-mcu1-legacy-fw/README.md`, Radxa commit `254d47e`).
Redistribution terms are therefore unknown; they are the same blobs the current Linux image already
ships (`PR/firmware/aic8800-intree.sh`). Treat as proprietary vendor firmware.

## 1. Hardware identities and roles

| Stick | VID:PID | Phase | Interfaces | Driver role |
|-------|---------|-------|------------|-------------|
| D80 | `a69c:8d80` | ROM loader (after storage-mode eject) | 1 vendor `ff/ff/ff` | loader: upload firmware, start app (`LF/aicwf_usb.c:aicloadfw_chipmatch` -> `PRODUCT_ID_AIC8800D80`) |
| D80 | `a69c:8d81`, clone `368b:8d81` | application firmware running | 3: BT `e0/01/01` + vendor Wi-Fi `ff/ff/ff` (`FD/aicwf_usb.c:aicwf_parse_usb` accepts 1 or 3 interfaces with `CONFIG_USB_BT`) | Wi-Fi on the vendor interface (`FD/aicwf_usb.c:aicwf_usb_chipmatch` -> `PRODUCT_ID_AIC8800D81`); BT interface for HCI (`RD/README.md`, `PR/firmware/patches/v0.39/linux/0003-btusb-...patch`) |
| DC | `a69c:88dc` | ROM firmware + RAM patches, PID never changes | 1 vendor `ff/ff/ff`, no `e0` class (project record "pluto-light-aic-btcoex": Wi-Fi only) | `PRODUCT_ID_AIC8800DC`; the Wi-Fi driver itself uploads patches (`FD/rwnx_main.c:rwnx_ic_system_init`) |

Both drivers match only the vendor interface (`USB_DEVICE_AND_INTERFACE_INFO(..., 0xff, 0xff, 0xff)`,
`PR/firmware/patches/aic8800/0001-...patch` hunks for `LF/aicwf_usb.c` and `FD/aicwf_usb.c` id tables).
Configuration checks: exactly 1 configuration, device class 0x00 (or 239 = IAD for the loader) (`LF/aicwf_usb.c:aicwf_parse_usb`).
A device that is not high/super speed is rebooted by the loader instead of loaded
(`LF/aicwf_usb.c:aicwf_usb_probe`, `system_reboot`); the Pluto host port is high speed.

## 2. USB transport

### 2.1 Endpoints (per vendor interface)

Endpoint addresses are not hard-coded; they are discovered in descriptor order
(`FD/aicwf_usb.c:aicwf_parse_usb`, `LF/aicwf_usb.c:aicwf_parse_usb`):

| Pipe | Rule | Use |
|------|------|-----|
| `bulk_out` | first bulk OUT | data TX; also commands when no msg OUT EP |
| `msg_out` | second bulk OUT (optional) | commands (`FD/aicwf_usb.c:aicwf_usb_bus_txmsg` uses it if present). Loader uses it only for 8801/8d81/8d91/8d99 (`LF/aicwf_usb.c:aicwf_parse_usb`, `use_msg_ep`), i.e. **not** for `8d80` |
| `bulk_in` | first bulk IN | data RX and (if no msg IN) command responses/events |
| `msg_in` | second bulk IN (optional, not used on 8801) | command responses/events (`FD/aicwf_usb.c:aicwf_usb_submit_msg_rx_urb`, `FD/aicwf_txrxif.c:aicwf_process_msg_rxframes`) |

No interrupt endpoints and no control transfers are used by the Wi-Fi function. RTEMS design: one
`usbd_transfer_setup()` config table with up to 4 bulk channels, picked with `.endpoint = UE_ADDR_ANY`
plus per-direction index, `.flags.short_xfer_ok=1` on IN and `.flags.force_short_xfer=1` on OUT
(ZLP, see 2.2) (`LB/sys/dev/usb/usbdi.h` `struct usb_xfer_flags`). Stage 0 must log the real
descriptors of all four PIDs (unknown in this repository).

### 2.2 OUT framing

Every OUT transfer starts with a 4-byte header; transfers whose length is a multiple of the max
packet size are terminated with a ZLP (`URB_ZERO_PACKET`: `FD/aicwf_usb.c:aicwf_usb_bus_txmsg`,
`aicwf_usb_bus_txdata`; `LF/aicwf_usb.c:aicwf_usb_bus_txmsg`). Without `CONFIG_USE_USB_ZERO_PACKET`
Linux would instead append one zero byte when `len % 512 == 0` (`FD/aicwf_usb.c:aicwf_usb_bus_txdata`).

Command (`FD/rwnx_cmds.c:aicwf_set_cmd_tx`, identical `LF/aicbluetooth_cmds.c:aicwf_set_cmd_tx`):

```
off  size
0    u16  len12 = (8 + param_len + 4) & 0x0fff   ; bytes after this 4-byte header
2    u8   type  = 0x11                           ; USB_TYPE_CFG_CMD_RSP
3    u8   0
4    u32  0 (dummy word)
8    u16  msg id          ; struct lmac_msg, little endian
10   u16  dest task id    ; TASK_xx
12   u16  src  task id    ; DRV_TASK_ID = 100 (FD/lmac_msg.h)
14   u16  param_len
16   ...  params (word aligned struct)
total transfer = 16 + param_len ; command buffer max 1536 (FD/aicwf_txrxif.h CMD_BUF_MAX)
```

Data (`FD/aicwf_usb.c:aicwf_usb_bus_txdata`):

```
0    u16  len12 = total transfer length INCLUDING this header, rounded up to 4 (TX_ALIGNMENT)
2    u8   type = 0x01 (data)
3    u8   0
4    struct hostdesc (28 bytes, 2.4)
32   payload = Ethernet frame without its 14-byte header (L3 payload)
```

Note the asymmetry: the command length excludes the 4-byte header, the data length includes it
(both cited functions above). Linux keeps 200 TX buffers; flow control = stop the net queue when
fewer than 50 are free (`FD/aicwf_usb.h` `AICWF_USB_TX_URBS`, `AICWF_USB_TX_LOW_WATER`;
`FD/aicwf_usb.c:aicwf_usb_bus_txdata` label `flow_ctrl`). The firmware credit messages exist
(`ME_TX_CREDITS_UPDATE_IND` -> `FD/rwnx_msg_rx.c:rwnx_rx_me_tx_credits_update_ind`) but credit
decrement is compiled out (`#if 0` around `txq->credits--` in `FD/rwnx_tx.c:rwnx_tx_push`):
**back-pressure is the USB bulk NAK plus the host buffer pool**. Only one command is in flight at a
time on the wire (`msg_busy` bit in `FD/aicwf_usb.c:aicwf_usb_bus_txmsg`).

### 2.3 IN framing and RX aggregation

Every IN element starts with `u16 len` (bytes 0..1) and a type byte (byte 2)
(`FD/aicwf_txrxif.c:aicwf_process_rxframes`, `FD/aicwf_usb.h` enum):

| byte2 & 0x7f | Meaning | Element size on the wire |
|---|---|---|
| bit 0x10 clear | data: `len` = 802.11 frame length; element = 60-byte RX header + frame | `len + 60`, **not** padded (aggregated path: `adjust_len = aggr_len`) |
| `0x11` CMD_RSP | firmware message, `struct ipc_e2a_msg` at offset 4 | `4 + roundup4(len)` |
| `0x12` DATA_CFM | TX confirmation: `u32 status, u32 index` at offset 4 (`FD/usb_host.c:aicwf_usb_host_tx_cfm_handler`) | `4 + roundup4(len)` |
| `0x13` PRINT | firmware log text (`FD/rwnx_msg_rx.c:rwnx_rx_handle_print`) | `4 + roundup4(len)` |

For data the first 4 bytes are not a separate header: they are the first word of `hw_vect`
(`len:16`, `reserved:8` = type) inside the RX header (`FD/rwnx_rx.h` `struct hw_vect`).

Aggregation: for `8d81` the driver sets `aicwf_usb_rx_aggr = true` and posts 20 RX transfers of
20480 bytes (`FD/aicwf_usb.c:aicwf_usb_chipmatch`, `aicwf_usb_submit_rx_urb`,
`AICWF_USB_AGGR_MAX_PKT_SIZE = 2048*10`); one transfer holds several elements, walked until
`len == 0` or the buffer ends (`FD/aicwf_txrxif.c:aicwf_another_ptk`, aggregated branch of
`aicwf_process_rxframes`). The D80 loader programs RX aggregation count 2 into the firmware
(`LF/aic_compat_8800d80.c` `patch_tbl_d80` entry `{0x0170, 2}` "rx aggr counter"). For `88dc`
aggregation is off: 2048-byte transfers, one element each (`aicwf_usb_max_pkt_size`).
The msg IN pipe uses 100 x 2048-byte transfers (`FD/aicwf_usb.h` `AICWF_USB_MSG_RX_URBS`).
**RTEMS: use one parser (aggregation-tolerant) for every IN pipe and both chips.**

`struct ipc_e2a_msg` (`FD/ipc_shared.h`): `u16 id; u16 dest; u16 src; u16 param_len; u32 pattern;
u32 param[256]`. Responses are matched to the waiting request by `id == reqid`
(`FD/rwnx_cmds.c:cmd_mgr_msgind`); unsolicited indications dispatch on `MSG_T(id)=id>>10`,
`MSG_I(id)=id&0x3ff` (`FD/lmac_msg.h`, `FD/rwnx_msg_rx.c` handler tables).
Command timeout 4000 ms (`FD/rwnx_cmds.h` `RWNX_80211_CMD_TIMEOUT_MS`), loader 2000 ms x queue depth
(`LF/aicbluetooth_cmds.c:cmd_mgr_queue`), bulk-OUT completion timeout 5000 ms (`CMD_TX_TIMEOUT`).
On a timeout the command manager is marked crashed; our Linux patch then resets the USB device
once (`PR/firmware/patches/aic8800/0001-...patch`: `aicwf_usb_recover()` via `usb_queue_reset_device`,
called from `FD/rwnx_cmds.c`). RTEMS equivalent: `usbd_req_re_enumerate()` / port reset and re-run 3.

### 2.4 TX descriptor `struct hostdesc` (28 bytes, `FD/ipc_shared.h`)

```
0  u16 packet_len        ; payload length (frame minus Ethernet header)
2  u16 flags_ext         ; 0
4  u32 status_desc_addr  ; 0 normally; (1<<31)|cfm_index when a DATA_CFM is wanted;
                         ; (1<<30)|rate_config for a fixed rate
8  u8  eth_dest[6]
14 u8  eth_src[6]
20 u16 ethertype         ; copied raw from the Ethernet header (network byte order)
22 u8  ac                ; hw queue: BK=0 BE=1 VI=2 VO=3 BCMC=4 (FD/rwnx_tx.h RWNX_HWQ_*)
23 u8  tid               ; 0xFF if not QoS
24 u8  vif_idx           ; from MM_ADD_IF_CFM.inst_nbr
25 u8  staid             ; AP station index from SM_CONNECT_IND.ap_idx (0xFF unknown)
26 u16 flags             ; TXU_CNTRL_*: MGMT=BIT3, MGMT_NO_CCK=BIT4, AMSDU=BIT6, ROBUST=BIT7, 4ADDR=BIT8
```

Filled in `FD/rwnx_tx.c:rwnx_start_xmit` (Ethernet header saved, `skb_pull(skb,14)`, descriptor
DA/SA/ethertype/staid/tid/vif) and `rwnx_tx_push` (`ac` = hw queue, `status_desc_addr`). A DATA_CFM
is requested only for EAPOL (`0x888e`), WAPI (`0x88b4`) and mgmt action/assoc-resp frames
(`FD/rwnx_tx.c:rwnx_tx_push`, USB branch). The firmware builds the 802.11 header, LLC/SNAP and
encryption itself.

### 2.5 RX header `struct hw_rxhdr` (56 bytes, frame at offset 60)

Layout (`FD/rwnx_rx.h` `struct hw_vect`, `rx_vector_1`, `phy_channel_info_desc`, `hw_rxhdr`;
offset 60 from `FD/rwnx_rx.c:rwnx_rxdataind_aicwf` `skb_pull(msdu_offset + 2)` with
`msdu_offset = sizeof(hw_rxhdr)+2` and `RX_HWHRD_LEN 60` in `FD/aicwf_txrxif.h`):

```
0   u16 len (frame length) | u8 type | u8 mpdu_cnt:6, ampdu_cnt:2
4   u32 tsf_lo, 8 u32 tsf_hi
12  rx_vector_1 (16 B): byte12 format_mod:4 ch_bw:3 pre_type:1; byte13 antenna_set;
    byte14 rssi_leg (s8); ...; byte17 rssi1 (s8)   [bit-field packing: verify at stage 3]
28  rx_vector_2 (8 B)
36  u32 status: rx_vect2_valid:1 resp_frame:1 decr_status:3 (0 none,1 WEP,2 TKIP,3 CCMP128,
    4 CCMP256,5 GCMP128,6 GCMP256,7 WAPI; FD/rwnx_rx.h RWNX_RX_HD_DECR_*) ...
40  phy_channel_info: u8 band, u8 chan_type, u16 prim20_freq, u16 center1, u16 center2
48  u32 flags: is_amsdu:1 is_80211_mpdu:1 is_4addr:1 new_peer:1 user_prio:1 need_reord:1
    upload:1 is_monitor_vif:1 vif_idx:8 sta_idx:8 dst_idx:8
52  u32 pattern
60  802.11 frame (decrypted, security header still present, MIC removed)
```

Processing in `FD/rwnx_rx.c:rwnx_rxdataind_aicwf`: frames without `flags_upload` are not
forwarded; data frames still carry the 802.11 header and the IV/ExtIV (pull 8 B for
CCMP/GCMP/TKIP, 4 for WEP, 18 for WAPI) and are converted to Ethernet by the host; host-side
de-fragmentation; `flags_need_reord` frames go through a host reorder buffer (`reord_process_unit`,
`AICWF_RX_REORDER`); `flags_is_80211_mpdu` frames are management frames for cfg80211
(`rwnx_rx_mgmt_any`).

## 3. Firmware bring-up

All bring-up uses four debug messages (task `TASK_DBG=1`, `FD/lmac_msg.h` `enum dbg_msg_tag`):

| Msg | ID | Params | CFM |
|-----|----|--------|-----|
| DBG_MEM_READ_REQ | 0x0400 | `u32 addr` | 0x0401 `{u32 addr; u32 data}` |
| DBG_MEM_WRITE_REQ | 0x0402 | `u32 addr; u32 data` | 0x0403 |
| DBG_MEM_BLOCK_WRITE_REQ | 0x040B | `u32 addr; u32 size; u32 data[256]` (param_len always 1032) | 0x040C |
| DBG_START_APP_REQ | 0x040D | `u32 bootaddr; u32 boottype` (AUTO=1, REBOOT=3, FNCALL=4, DUMMY=5) | none awaited (`reqcfm=0`) |
| DBG_MEM_MASK_WRITE_REQ | 0x0411 | `u32 addr; u32 mask; u32 data` | 0x0412 |

(`LF/aicbluetooth_cmds.c:rwnx_send_dbg_*`, `LF/aicbluetooth_cmds.h` structs, `FD/lmac_msg.h` `HOST_START_APP_*`.)
Image upload = consecutive block writes; loader chunks 1024 B (`LF/aicbluetooth.c:rwnx_plat_bin_fw_upload_android`),
the Wi-Fi driver 512 B (`FD/rwnx_platform.c:rwnx_plat_bin_fw_upload_2`, l.837). The last chunk may be
any length (`fmacfw_8800d80_u02.bin` is 327037 B, not a multiple of 4).

Chip identification (both chips): read `0x40500000`; `chip_id = (v >> 16) & 0xff`
(U01=0x01, U02=0x03, U03=0x07, `LF/aicwf_usb.h` `CHIP_REV_*`); `chip_mcu_id = 1` when bit 25 is 0;
"H" variant when `(chip_id & 0xC0) == 0xC0` (`LF/aic_compat_8800d80.c:system_config_8800d80`,
`LF/aic_compat_8800d80.h` `IS_CHIP_ID_H`). If `chip_mcu_id == 1`: read-modify-write `0x40100020 |= 1`
(cache enable) (same function; `FD/aicwf_compat_8800dc.c:system_config_8800dc`). DC additionally:
`chip_sub_id = read(0x00000020) & 0xff` (0 = U01, 1 = U02, 2 = U02H) (`FD/aicwf_compat_8800dc.c:system_config_8800dc`).
The `legacy-mcu1` branch is only for `chip_mcu_id=1` units (`RD/README.md` header); log both ids at
stage 1 and refuse to continue on `chip_mcu_id=0` until the `main`-branch firmware set is evaluated.

### 3.1 D80 sequence (on `a69c:8d80`)

`LF/aicwf_usb.c:aicwf_usb_probe` -> `system_config` -> `aicfw_download_fw` ->
`LF/aic_compat_8800d80.c:aicfw_download_fw_8800d80` (normal mode, `chip_id != U01`):

1. If the device enumerates directly as `8d81` and firmware was not loaded in this boot: send
   `DBG_START_APP_REQ(bootaddr=2000, REBOOT)` and wait for it to come back as `8d80`
   (`LF/aicwf_usb.c:aicwf_usb_probe`, `LF/aicbluetooth_cmds.c:rwnx_send_reboot`).
2. Read `0x40500000` -> chip/mcu id; mcu1: `0x40100020 |= 1`. `syscfg_tbl_8800d80` is empty
   without `CONFIG_PMIC_SETTING` (`LF/aic_compat_8800d80.c:system_config_8800d80`).
3. Parse `fw_patch_table_8800d80_u02.bin` (`LF/aicbluetooth.c:aicbt_patch_table_alloc`): 16-byte
   magic `"AICBT_PT_TAG"`, then records `{char name[16]; u32 type; u32 len; u32 pairs[len][2]}`
   (records with `type >= 1000` carry no data). Our file contains: `PINF` (type 0: adid 0x201940,
   patch 0x1E0000), `TRAP` (1, 19 pairs), `PATCH_TB4` (2, 42), `MODE` (3, 9), `POWER_ON` (4, 3),
   `PATCH_TAF` (5, 15), `VER_INFO` (6, text) (decoded from `FW/aic8800D80/fw_patch_table_8800d80_u02.bin`).
   `aicbt_patch_info_unpack` takes the adid/patch addresses from `PINF` (overriding the
   `FW_RAM_*_8800D80_U02` defaults 0x201940/0x20B43C in `LF/aic_compat_8800d80.h`); ext patches = 0.
4. Block-write `fw_adid_8800d80_u02.bin` -> 0x00201940, `fw_patch_8800d80_u02.bin` -> 0x001E0000.
5. Patch table load (`LF/aicbluetooth.c:aicbt_patch_table_load`): for every record except type 6,
   `DBG_MEM_WRITE(pair[0], pair[1])`; the `MODE` record values are overwritten first with
   `[1]=hwinfo<0 (1), [3]=hwinfo (-1), [5]=cpmode (0), [7]=btmode (5), [9]=btport (1=MB),
   [11]=uart_baud (1500000), [13]=uart_fc (1), [15]=lpm (0), [17]=bt txpwr (0x6F2F)`
   (`aicbt_info[PRODUCT_ID_AIC8800D80]` in `LF/aicbluetooth.c`); after `POWER_ON` wait 100 ms.
6. Block-write `fmacfw_8800d80_u02.bin` (or `fmacfw_8800d80_h_u02.bin` on H chips: **not present in
   `FW/aic8800D80`**, so H units fail) -> 0x00120000 (`RAM_FMAC_FW_ADDR_8800D80_U02`).
7. `aicwf_patch_config_8800d80`: `config_base = read(0x120000+0x198)`, `patch_str = read(0x120000+0x1A0)`;
   write `patch_str+0 = 0x48435450 ("PTCH")`, `+8 = 0x50544348`, `+4 = 0x001D7000` (pair table),
   `+12 = count`; pairs at 0x1D7000: `{config_base+0x00B4, 0xF3010001 (USE_5G)}`,
   `{config_base+0x0170, 2}`; block_size[0..3] (`+0x20..+0x2C`) = 0 (`LF/aic_compat_8800d80.c`).
8. `DBG_START_APP_REQ(0x00120000, AUTO)`. The device detaches and re-enumerates as `8d81`
   (`PR/firmware/patches/aic8800/...patch` comment in the loader probe hunk).

No userconfig is uploaded by the D80 loader (the generic `rwnx_plat_userconfig_upload_android`
is only in the 8800 path, `LF/aicwf_usb.c:aicfw_download_fw_8800`). Total ~355 KB, ~330 block writes.

### 3.2 D80 application start (on `8d81`, Wi-Fi driver)

`FD/rwnx_main.c:rwnx_cfg80211_init`:
1. `rwnx_ic_system_init`: `system_config_8800d80` (re-read chip id; `FD/aicwf_compat_8800d80.c`),
   parse `aic_userconfig_8800d80.txt` (`rwnx_plat_userconfig_load_8800d80` ->
   `FD/rwnx_platform.c:rwnx_plat_userconfig_parsing`): `enable`, `lvl_*` per-rate TX power
   (2.4/5 GHz, 11b/ag/n/ac/ax), `ofst_*` per-channel-group offsets, `xtal_enable/xtal_cap/xtal_cap_fine`.
2. `MM_SET_STACK_START_REQ {is_stack_start=1, efuse_valid=0, set_vendor_info=BIT5, fwtrace_redir}`
   -> CFM `{is_5g_support, vendor_info}`; 5 GHz band enabled from `is_5g_support`.
3. `MM_GET_FW_VERSION_REQ` (0x0080).
4. `rwnx_ic_rf_init` -> `FD/aicwf_compat_8800d80.c:aicwf_set_rf_config_8800d80`:
   `MM_SET_TXPWR_IDX_LVL_REQ` (v3 table from `lvl_*`), `MM_SET_TXPWR_LVL_ADJ_REQ`,
   `MM_SET_TXPWR_OFST_REQ` (2x ofst table), `MM_SET_RF_CALIB_REQ` (`cal_cfg_24g/5g`, `xtal_cap`,
   `FD/rwnx_msg_tx.c:rwnx_send_rf_calib_req`).
5. `MM_GET_MAC_ADDR_REQ {u32 get}` -> `{u8 mac[6]}`; all-zero -> random MAC
   (`CONFIG_MAC_RANDOM_IF_NO_MAC_IN_EFUSE`).
6. `MM_RESET_REQ`, `MM_SET_VENDOR_SWCONFIG_REQ` (temp comp), `MM_VERSION_REQ`.
7. `ME_CONFIG_REQ` (HT/VHT/HE caps, `ps_on`, `tx_lft`, `phy_bw_max`), then after regulatory
   `ME_CHAN_CONFIG_REQ` (channel list) (`FD/rwnx_main.c:rwnx_cfg80211_init` l.9381, l.9408).

### 3.3 DC sequence (on `a69c:88dc`, all in the Wi-Fi driver)

`FD/rwnx_main.c:rwnx_ic_system_init` (DC branch) then `rwnx_ic_rf_init`:
1. `aicwf_usb_config`: if `read(0x40200024) & 0xffff == 0x119` write `0x40200028=0x0021047E`,
   `0x40200024=0x0000011D`.
2. `system_config_8800dc`: chip/mcu/sub id (above), `set_bbpll_config`, `syscfg_tbl_8800dc` writes,
   masked writes (`syscfg_tbl_masked_8800dc[_h]`, `_u01`) (`FD/aicwf_compat_8800dc.c`).
3. `rwnx_platform_on` -> `rwnx_plat_patch_load` (`FD/rwnx_platform.c`):
   `aicwf_plat_patch_load_8800dc`: `fmacfw_patch_8800dc_u02.bin` (sub_id 1) or `_h_u02.bin` (sub_id 2)
   -> `ROM_FMAC_PATCH_ADDR 0x00180000`; BT patches are not loaded (`CONFIG_LOAD_BT_PATCH_IN_FDRV` off).
4. `aicwf_patch_config_8800dc`: read `cfg_base` words, write `patch_tbl_wifisetting` relative to the
   wifisetting address, block-write LDPC and AGC tables and the TX gain map (512-B chunks), jump/func
   patch writes, then `aicwf_patch_table_load(fmacfw_patch_tbl_8800dc[_h]_u02.bin)`: word 0 = address,
   block-write the first 128 bytes there, then `DBG_MEM_WRITE` pairs from byte 128 on; U02: write
   `0x00111944 = 0x101`; H: `aicwf_patch_var_config_8800dc` (`FD/aicwf_compat_8800dc.c`).
5. DPD calibration on every start (`CONFIG_FORCE_DPD_CALIB`, `PR/firmware/aic8800-intree.sh`):
   `aicwf_dpd_calib_8800dc`: skip if misc RAM already valid; block-write `fmacfw_calib_8800dc[_h]_u02.bin`
   -> `0x00130000`, `DBG_START_APP_REQ(0x00130009, FNCALL)`, read the result words back from the misc
   RAM (`read(0x10164+0x14)` = base).
6. Userconfig `aic_userconfig_8800dc.txt` (or the `dw` variants) (`FD/aicwf_compat_8800dc.c:rwnx_plat_userconfig_load_8800dc/_8800dw`).
7. `wdt_config` (sub_id < 2: masked write `0x4010300C` bit0), `start_from_bootrom`: read `0x00150000`,
   `DBG_START_APP_REQ(0x00150000, DUMMY)` (`FD/rwnx_main.c:start_from_bootrom`, `RAM_LMAC_FW_ADDR`).
8. `MM_SET_STACK_START_REQ {1, 0, 0, fwlog}` (DC branch), then `aicwf_set_rf_config_8800dc`:
   `MM_SET_TXPWR_IDX_LVL_REQ`, `MM_SET_TXPWR_OFST_REQ`, `MM_SET_RF_CONFIG_REQ` tables, `MM_SET_RF_CALIB_REQ`.
9. Same 3.2 steps 5..7. At first interface open DC also writes RX gain: masked write
   `0x4033B300` mask 0xFF = 0x0E (`FD/rwnx_main.c:rwnx_open`).

### 3.4 Firmware files for the RTEMS image

Station mode only (`PR/firmware/aic8800-intree.sh` lists exactly what the Linux image installs):

| Chip | File | Size (B) | Target |
|------|------|---------|--------|
| D80 | `fw_patch_table_8800d80_u02.bin` | 984 | parsed (3.1/3) |
| D80 | `fw_adid_8800d80_u02.bin` | 1708 | 0x201940 |
| D80 | `fw_patch_8800d80_u02.bin` | 25300 | 0x1E0000 |
| D80 | `fmacfw_8800d80_u02.bin` | 327037 | 0x120000 |
| D80 | `aic_userconfig_8800d80.txt` | 2448 | host-parsed |
| DC | `fmacfw_patch_8800dc_u02.bin` / `_h_u02` | 23356 / 26116 | 0x180000 |
| DC | `fmacfw_patch_tbl_8800dc_u02.bin` / `_h_u02` | 504 / 744 | per table |
| DC | `fmacfw_calib_8800dc_u02.bin` / `_h_u02` | 40428 / 32684 | 0x130000 |
| DC | `aic_userconfig_8800dc.txt` (+ `dw`, `dw_u2`, `dw_w311`) | 1138 each | host-parsed |

Not needed: `lmacfw_rf_*` (RF test), `*_ipc*` (IP camera), `fw_ble_scan_ad_filter.bin`, `calibmode_8800d80.bin`
(`PR/firmware/aic8800-intree.sh` comments). Keep the build-time TX power cap (every `lvl_*` <= 15 dBm,
D80 `loss_*` keys removed; `PR/firmware/aic8800-intree.sh:cap_userconfig`). Embed with libbsd firmware(9):
`LB/../firmware-gen.py` generates a `firmware_register()` C file per blob (pattern used by
`LB/../rtemsbsd/local/runfw.c`); `firmware_get("aic8800d80_fmacfw")` at attach. Size ~485 KB in total.

## 4. LMAC/UMAC message API for station mode

IDs = `(task << 10) | index` (`FD/lmac_msg.h` `LMAC_FIRST_MSG`); tasks MM=0, DBG=1, SCAN=2, TDLS=3,
SCANU=4, ME=5, SM=6, APM=7 (`FD/lmac_msg.h` task enum). Values below are computed from the enum order
in `FD/lmac_msg.h` and must match the matched SDK V3 firmware of this branch. Sizes/offsets were
checked by compiling `FD/lmac_msg.h` (natural alignment, `bool_l`/`u8_l` = 1 byte).

| Function | REQ | CFM / IND | Params (bytes) | Linux sender |
|---|---|---|---|---|
| reset | MM_RESET 0x0000 | 0x0001 | none | `FD/rwnx_msg_tx.c:rwnx_send_reset` |
| start | MM_START 0x0002 | 0x0003 | 72: `u32 phy_cfg[16]; u32 uapsd_timeout(300); u16 lp_clk_ppm(20)` | `rwnx_send_start` (first vif open, `FD/rwnx_main.c:rwnx_open`) |
| version | MM_VERSION 0x0004 | 0x0005 (28 B) | none | `rwnx_send_version_req` |
| add vif | MM_ADD_IF 0x0006 | 0x0007 `{u8 status; u8 inst_nbr}` | 10: `u8 type (MM_STA=0); u8 mac[6] @2; bool p2p @8` | `rwnx_send_add_if` |
| remove vif | MM_REMOVE_IF 0x0008 | 0x0009 | `u8 inst_nbr` | `rwnx_send_remove_if` |
| key add | MM_KEY_ADD 0x0024 | 0x0025 `{u8 status; u8 hw_key_idx}` | 44: `u8 key_idx; u8 sta_idx (0xFF=group); {u8 len; u32 key[8]} @4; u8 cipher @40; u8 inst_nbr; u8 spp; bool pairwise @43` | `rwnx_send_key_add` |
| key del | MM_KEY_DEL 0x0026 | 0x0027 | `u8 hw_key_idx` | `rwnx_send_key_del` |
| TX power | MM_SET_POWER 0x002E | 0x002F `{u8 radio; s8 power}` | `u8 inst_nbr; s8 dBm` | `rwnx_send_set_power` (patched: clamp to userconfig max) |
| RSSI threshold | MM_CFG_RSSI 0x0056 | MM_RSSI_STATUS_IND 0x0057 `{u8 vif; bool low; s8 rssi}` | `u8 vif; s8 thold; u8 hyst` | `rwnx_send_cfg_rssi_req` |
| coex | MM_SET_COEX 0x0065 | 0x0066 | 16: `u8 bt_on; u8 disable_coexnull; u8 enable_nullcts; u8 periodic; u8 ts_set; u32 ts[2] @8` | `rwnx_send_coex_req` |
| MAC addr | MM_GET_MAC_ADDR 0x0073 | 0x0074 `{u8 mac[6]}` | `u32 get` | `rwnx_send_get_macaddr_req` |
| station stats | MM_GET_STA_INFO 0x0075 | 0x0076 (32 B: `u32 rate_info; u32 txfailed; u8 rssi; ...chan_time/busy, ack_fail/succ`) | `u8 sta_idx` | `rwnx_send_get_sta_info_req` |
| stack start | MM_SET_STACK_START 0x007B | 0x007C `{u8 is_5g_support; u8 vendor_info}` | `u8 on; u8 efuse_valid; u8 set_vendor_info; u8 fwtrace_redir` | `rwnx_send_set_stack_start_req` |
| fw version | MM_GET_FW_VERSION 0x0080 | 0x0081 | none | `rwnx_send_get_fw_version_req` |
| TX power tables | 0x0077 / 0x0089 / 0x0079 | +1 | from userconfig | `rwnx_send_txpwr_lvl_v3_req`, `_lvl_adj_req`, `_ofst2x_req` |
| RF calib | MM_SET_RF_CALIB 0x0069 | 0x006A | 24: `u32 cal_cfg_24g, cal_cfg_5g, alpha, bt_calib_en, bt_calib_param; u8 xtal_cap, xtal_cap_fine` | `rwnx_send_rf_calib_req` |
| fw errors | - | MM_FW_PANIC_IND 0x0095, MM_FW_ASSERT_IND 0x0096, DBG_ERROR_IND 0x0408 | - | `FD/rwnx_msg_rx.c` tables |
| capabilities | ME_CONFIG 0x1400 | 0x1401 | 112: HT cap, VHT cap, HE cap, `u16 tx_lft; u8 phy_bw_max; bool ht/vht/he_supp, he_ul_on, ps_on, ant_div_on, dpsm` | `rwnx_send_me_config_req` |
| channels/country | ME_CHAN_CONFIG 0x1402 | 0x1403 | 254: `mac_chan_def chan2G4[14]; chan5G[28] @84; u8 cnt2G4 @252; u8 cnt5G` with `mac_chan_def {u16 freq; u8 band (0=2.4,1=5); u8 flags (NO_IR=1, DISABLED=2, RADAR=4); s8 tx_power}` | `rwnx_send_me_chan_config_req` (`FD/lmac_mac.h` chan flags) |
| control port | ME_SET_CONTROL_PORT 0x1404 | 0x1405 | `u8 sta_idx; bool open` | `rwnx_send_me_set_control_port_req` (from `rwnx_cfg80211_change_station`, AUTHORIZED) |
| TKIP MIC fail | - | ME_TKIP_MIC_FAILURE_IND 0x1406 | - | `rwnx_rx_me_tkip_mic_failure_ind` |
| power save | ME_SET_PS_MODE 0x1413 | 0x1414 | `u8 ps_state` (OFF=0, ON=1, ON_DYN=2) | `rwnx_send_me_set_ps_mode` |
| scan | SCANU_START 0x1000 | REQ ack = SCANU_START_CFM_ADDTIONAL 0x1009; results SCANU_RESULT_IND 0x1004; end SCANU_START_CFM 0x1001 `{u8 vif; u8 status; u8 result_cnt}` | 376: `mac_chan_def chan[42]; mac_ssid ssid[3] @252 ({u8 len; u8 ssid[32]}); u8 bssid[6] @352; u32 add_ies; u16 add_ie_len; u8 vif_idx @366; u8 chan_cnt; u8 ssid_cnt; bool no_cck; u32 duration @372` | `rwnx_send_scanu_req`; extra probe IEs first via SCANU_VENDOR_IE_REQ 0x1007 (260: `u16 len; u8 vif; u8 ie[256]`) |
| scan cancel | SCANU_CANCEL 0x100A | 0x100B | - | `rwnx_send_scanu_cancel_req` |
| connect | SM_CONNECT 0x1800 | 0x1801 `{u8 status}`; SM_CONNECT_IND 0x1802 | 320: `mac_ssid @0; bssid @34 (ff.. = any); mac_chan_def @40 (freq 0xFFFF = any); u32 flags @48; u16 ctrl_port_ethertype @52; u16 ie_len @54; u16 listen_itv; bool dont_wait_bcmc; u8 auth_type @59 (0 open,1 shared,2 FT,3 SAE); u8 uapsd_queues; u8 vif_idx @61; u32 ie_buf[64] @64` | `rwnx_send_sm_connect_req` |
| disconnect | SM_DISCONNECT 0x1803 | 0x1804; SM_DISCONNECT_IND 0x1805 `{u16 reason; u8 vif; bool ft_over_ds; u8 reassoc}` | `u16 reason; u8 vif_idx` | `rwnx_send_sm_disconnect_req` |
| SAE external auth | - | SM_EXTERNAL_AUTH_REQUIRED_IND 0x1806 | RSP 0x1807 | (skip: WPA3) |

`SM_CONNECT_IND` (852 B): `u16 status_code; u8 bssid[6] @2; bool roamed; u8 vif; u8 ap_idx @10;
u8 ch_idx; bool qos; u8 acm; u16 assoc_req_ie_len @14; u16 assoc_rsp_ie_len; u32 assoc_ie_buf[200] @20
(req IEs then rsp IEs); u16 aid @820; u8 band; u16 center_freq @824; u8 width; u32 cf1, cf2; u32 ac_param[4]`.
`ap_idx` becomes the station index for TX (`hostdesc.staid`), keys and control port
(`FD/rwnx_msg_rx.c:rwnx_rx_sm_connect_ind`). `SCANU_RESULT_IND`: `u16 length; u16 framectrl;
u16 center_freq; u8 band; u8 sta_idx; u8 inst_nbr; s8 rssi @9; payload @12` = the complete
beacon/probe-response frame (`FD/rwnx_msg_rx.c:rwnx_rx_scanu_result_ind` treats it as `ieee80211_mgmt`).

SM_CONNECT flags (`FD/lmac_mac.h` `enum mac_connection_flags`): `CONTROL_PORT_HOST=1`,
`CONTROL_PORT_NO_ENC=2`, `DISABLE_HT=4` (TKIP/WEP pairwise), `WPA_WPA2_IN_USE=8`, `MFP_IN_USE=16`,
`REASSOCIATION=32` (`FD/rwnx_msg_tx.c:rwnx_send_sm_connect_req`).
Cipher ids (`FD/lmac_mac.h`): WEP40=0, TKIP=1, CCMP=2, WEP104=3, SMS4=4, BIP_CMAC_128=5, GCMP_128=6,
GCMP_256=7, CCMP_256=8 (`FD/rwnx_main.c:rwnx_cfg80211_add_key` mapping).

Who does what (fullmac, `CONFIG_RWNX_FULLMAC`): the firmware SME performs authentication and
association (`SM_CONNECT_REQ` carries SSID/BSSID/channel/auth type/RSN IE from the supplicant,
`FD/rwnx_main.c:rwnx_cfg80211_connect`). The 4-way and group handshakes run on the **host**:
EAPOL frames arrive as ordinary RX data frames (ethertype 0x888e) and are sent as ordinary TX data
with a DATA_CFM requested (2.4); keys come back via `MM_KEY_ADD_REQ`; with `CONTROL_PORT_HOST` the
firmware blocks non-EAPOL data until `ME_SET_CONTROL_PORT_REQ(open=1, sta_idx=ap_idx)`.
Beacon loss: there is no handler for `MM_CONNECTION_LOSS_IND` (0x0043) in `FD/rwnx_msg_rx.c`
`mm_hdlrs`; link loss is reported as `SM_DISCONNECT_IND` (loss threshold in the firmware config,
cf. `linkloss_thd` in `LF/aic_compat_8800d80.c` `adaptivity_patch_tbl_d80`).

## 5. Data path

TX (2.2, 2.4): the firmware takes **802.3 semantics** (DA, SA, ethertype in the descriptor, L3 payload
after it) and does 802.11 encapsulation, QoS, sequence numbers, A-MPDU and encryption.
RX (2.3, 2.5): the firmware delivers **802.11 frames**, already decrypted, IV still in place,
MIC removed, plus RX vector (RSSI, rate) and flags; reordering and de-fragmentation are host jobs.

RTEMS RX: build an mbuf from offset 60, length `hw_vect.len`; for data frames attach
`struct ieee80211_rx_stats` (`c_freq` = `phy_prim20_freq`, `c_rssi`/`c_nf` from `rssi1`/`rssi_leg`,
`c_pktflags = IEEE80211_RX_F_DECRYPTED | IEEE80211_RX_F_MMIC_STRIP | IEEE80211_RX_F_MIC_STRIP |
IEEE80211_RX_F_PN_VALIDATED` when `decr_status != 0`) and call `ieee80211_input_mimo(ni, m)`
(`LB/sys/net80211/_ieee80211.h` RX flags, l.568-577; pattern `LB/sys/dev/usb/wlan/if_rsu.c:rsu_rx_multi_frame`).
net80211 then strips the IV (only `DECRYPTED`, not `IV_STRIP`), handles A-MSDU, EAPOL and delivers to
`ifnet`/BPF. Drop frames without `flags_upload`; route `flags_is_80211_mpdu` frames (mgmt) to
`ieee80211_input_mimo_all` only in SCAN/AUTH states (stage 6 decides if any are needed).
Reordering: the firmware negotiates ADDBA itself and gives no BA event to the host; start a
net80211 RX BA session on the first `need_reord` frame of a TID with
`ieee80211_ampdu_rx_start_ext(ni, tid, seq, 64)` (as `if_rsu.c:rsu_event_addba_req_report` does) or,
minimum viable, deliver in order of arrival (correct but slower TCP).

RTEMS TX (`ic_transmit`): net80211 hands over 802.11 frames (`LB/sys/net80211/ieee80211_output.c:ieee80211_encap`).
Undo the encapsulation: `hdrlen = ieee80211_anyhdrsize()`, skip the cipher header if
`IEEE80211_FC1_PROTECTED` (do not call `ieee80211_crypto_encap`), check LLC/SNAP `AA AA 03 00 00 00`,
take ethertype; DA = addr3, SA = addr2 (STA, ToDS); TID from QoS control, `ac` = `TID_TO_WME_AC`;
`staid` = AP index, `vif_idx` from add-if. Declaring `IEEE80211_CRYPTO_TKIPMIC` keeps the TKIP MIC out of
software (`IEEE80211_KEY_SWMIC` not set). Raw/mgmt TX (`ic_raw_xmit`, `ic_send_mgmt`): refuse
(`ENOTSUP`) like `if_rsu.c:rsu_send_mgmt`; the firmware sends auth/assoc/probe itself. Set
`IEEE80211_FEXT_SEQNO_OFFLOAD` (firmware numbers frames; `LB/sys/net80211/ieee80211_var.h` l.697).

## 6. Mapping onto net80211 (model: `if_rsu`)

`if_rsu` is the right model: firmware-run join, firmware site survey, scan offload, hardware keys
(`LB/sys/dev/usb/wlan/if_rsu.c:rsu_attach`, `rsu_newstate`, `rsu_event_survey`, `rsu_event_join_bss`).

Attach (`aic_attach`, per vendor interface; one `device_t` for `8d80` = loader only, detaches after start-app):
- `ic_caps = IEEE80211_C_STA | IEEE80211_C_WPA | IEEE80211_C_WME | IEEE80211_C_SHPREAMBLE |
  IEEE80211_C_SHSLOT` (no MONITOR, HOSTAP, IBSS, PMGT; `rsu_attach` set minus monitor/bgscan).
- `ic_cryptocaps = IEEE80211_CRYPTO_WEP | IEEE80211_CRYPTO_TKIP | IEEE80211_CRYPTO_AES_CCM |
  IEEE80211_CRYPTO_TKIPMIC` (+ `AES_GCM_128` later) (`LB/sys/net80211/ieee80211_crypto.h` l.153-164).
- `ic_htcaps = IEEE80211_HTC_HT | HTCAP_SHORTGI20 | HTCAP_SHORTGI40 | HTCAP_CHWIDTH40 | IEEE80211_HTC_AMPDU`;
  1x1 streams (Linux advertises `nss` 2 by default, `FD/rwnx_mod_params.c`; read the real value from
  `MM_VERSION_CFM.features`/fw before advertising). VHT later.
- `ic_flags_ext |= IEEE80211_FEXT_SCAN_OFFLOAD | IEEE80211_FEXT_SEQNO_OFFLOAD` (`if_rsu.c` l.582).
- `ic_getradiocaps`: 2.4 GHz channels 1-13 always, 5 GHz only if `MM_SET_STACK_START_CFM.is_5g_support`.
- MAC from `MM_GET_MAC_ADDR_CFM` into `ic_macaddr`.

Callbacks:

| net80211 hook | AIC action |
|---|---|
| `ic_vap_create` / `ic_vap_delete` | `ieee80211_vap_setup`; `MM_ADD_IF(MM_STA, mac)` -> `inst_nbr`; first vif: `MM_START`, `MM_SET_COEX(1,0,1)` (D80), DC RX-gain write (3.3/9); delete: `MM_REMOVE_IF` |
| `ic_parent` | up: run sections 3.2/3.3 if not yet, `ME_CONFIG(ps_on=0)`, `ME_CHAN_CONFIG`, `MM_SET_POWER` from `ic_txpowlimit` (clamped to userconfig cap 15 dBm); down: `SM_DISCONNECT`, stop transfers |
| `ic_scan_start` | build `SCANU_START_REQ` from `ic->ic_scan` (channels, SSIDs from `ss_ssid`); IEs from `vap->iv_appie_probereq` via `SCANU_VENDOR_IE_REQ` |
| `SCANU_RESULT_IND` | mbuf of the frame + `ieee80211_rx_stats` (freq, rssi) -> `ieee80211_input_mimo_all` (exactly `rsu_event_survey`) |
| `SCANU_START_CFM` (0x1001) | `ieee80211_scan_done(vap)` (`rsu_rx_event`) |
| `ic_scan_end`, `ic_set_channel`, `ic_scan_curchan`, `ic_scan_mindwell` | no-ops (`rsu_*` equivalents) |
| `iv_newstate` -> `IEEE80211_S_AUTH` | `SM_CONNECT_REQ`: SSID/BSSID/channel from `vap->iv_bss`, `ie_buf` = `vap->iv_appie_wpa` (set by the supplicant via `IEEE80211_IOC_OPTIE`), flags `CONTROL_PORT_HOST|WPA_WPA2_IN_USE` when an RSN/WPA IE is present, `ctrl_port_ethertype = htons(0x888e)` raw, `auth_type` from `iv_bss->ni_authmode` |
| `SM_CONNECT_IND` status 0 | save `ap_idx`, `aid`, QoS; `ieee80211_new_state(vap, IEEE80211_S_RUN, IEEE80211_FC0_SUBTYPE_ASSOC_RESP)` (`rsu_event_join_bss`); open network: `ME_SET_CONTROL_PORT(ap_idx, 1)` immediately |
| `SM_CONNECT_IND` status != 0, `SM_DISCONNECT_IND` | `ieee80211_new_state(vap, IEEE80211_S_SCAN, -1)` (`rsu_event_join_bss`/`rsu_rx_event` disconnect handling) |
| `iv_newstate` leaving RUN | `SM_DISCONNECT_REQ(reason 3)` (`FD/rwnx_main.c:rwnx_close` uses reason 3) |
| `iv_key_alloc` | keyix: WEP index for group keys, 0 for pairwise (`rsu_key_alloc`) |
| `iv_key_set` | `MM_KEY_ADD`: pairwise -> `sta_idx=ap_idx, pairwise=1`; group -> `sta_idx=0xFF, key_idx=k->wk_keyix`; TKIP key = 16 B TK + 8 B TX MIC + 8 B RX MIC, same order as Linux `params->key` (net80211 `wk_key`, `wk_txmic`, `wk_rxmic` contiguous); keep `hw_key_idx`. After pairwise **and** group key of a WPA association: `ME_SET_CONTROL_PORT(ap_idx, 1)` (Linux does this on `NL80211_STA_FLAG_AUTHORIZED`, which the bsd supplicant driver never sends) |
| `iv_key_delete` | `MM_KEY_DEL(hw_key_idx)` |
| `ic_setregdomain` | `ME_CHAN_CONFIG` from the new channel list (`ic_channels`, max power, DFS/passive -> `CHAN_RADAR`/`CHAN_NO_IR`) |
| `ic_update_mcast`, `ic_update_promisc` | no-op (firmware filters) |
| node RSSI / stats | per-frame RSSI via rx_stats; optional periodic `MM_GET_STA_INFO(ap_idx)` for `IEEE80211_IOC_STA_STATS` |
| `ME_TKIP_MIC_FAILURE_IND` | `ieee80211_notify_michael_failure` |

wpa_supplicant (`LB/contrib/wpa/src/drivers/driver_bsd.c`, already built with `CONFIG_DRIVER_BSD=1`
in `LB/../libbsd.py`, and enabled in `PR/rtos/libbsd/pluto.ini`): it sets `IEEE80211_IOC_ROAMING`
manual, `IEEE80211_IOC_WPA`/`PRIVACY`/`OPTIE` (RSN IE), scans with `IEEE80211_IOC_SCAN_REQ` and reads
`IEEE80211_IOC_SCAN_RESULTS`, associates with `IEEE80211_IOC_MLME` (`MLME_ASSOC`), installs keys with
`IEEE80211_IOC_WPAKEY`, sends/receives EAPOL through the BPF l2_packet on the interface, and reacts to
`RTM_IEEE80211_ASSOC/DISASSOC/SCAN/LEAVE` routing messages that net80211 emits on the state changes
above. No driver-specific ioctl is needed. MLME_ASSOC drives net80211 to `S_AUTH`, which is where the
driver issues `SM_CONNECT_REQ`.

Minimum viable feature set: STA, open + WPA2-PSK (CCMP, TKIP group), 2.4 GHz (D80 and DC) + 5 GHz
(D80 if `is_5g_support`), 20/40 MHz HT, country via `ifconfig wlan0 country XX` ->
`ME_CHAN_CONFIG`, TX power cap, power save off (`PR/firmware/br-external/board/nano/rootfs-overlay/etc/modprobe.d/aic8800.conf`
`ps_on=0`), DHCP via dhcpcd. Skip: AP/hotspot (`light-ap` role), P2P, mesh, TDLS, monitor, WPA3/SAE
external auth, 11ac/11ax rate reporting, ARP offload, WoWLAN, RF test mode, BLE-wakeup firmware.

## 7. Bluetooth on the D80

Linux: no AIC Bluetooth driver; `aic_load_fw` uploads the BT patch (adid, patch, patch table incl.
BT mode) through the Wi-Fi loader before starting the application, after which the standard `btusb`
binds to interface class `e0/01/01` (`RD/README.md` "Bluetooth on Combo Adapters"; `RD/diagnose_bt.sh`;
`LF/aic_compat_8800d80.c:aicfw_download_fw_8800d80`). The legacy `aic_btusb` module is not used.
**No HCI vendor patch download is required**: BT works only after section 3.1 has run, and it is
reset together with the Wi-Fi firmware (`RD/README.md` known limitation: BT may need a replug after
a Wi-Fi reset). The DC exposes no BT interface.

Quirk: ACL bulk OUT transfers need a zero-length packet when the length is a multiple of the max
packet size; without it AAC A2DP stalled after about a minute (`RD/tests/issue63-zlp-quirk/README.md`;
our kernel patch `PR/firmware/patches/v0.39/linux/0003-btusb-aic8800d80-368b-8d81-acl-tx-zlp.patch`
sets `URB_ZERO_PACKET` for `368b:8d81` interface `e0/01/01`). Apply it to both `a69c:8d81` and
`368b:8d81` (harmless with a correct controller).

RTEMS HCI-USB (H2) transport for BTstack (libbsd has no netgraph/`ng_ubt`: `LB/sys/netgraph` holds only
`ng_socket.h`), as a libbsd USB driver matching `e0/01/01` on `a69c|368b:8d81`:
- HCI command: control transfer `bmRequestType=0x20` (class, interface), `bRequest=0`, `wIndex` = interface
  number, data = command packet (USB Bluetooth transport convention as used by `btusb`).
- HCI event: interrupt IN endpoint of interface 0, reassemble events by the 2-byte header length.
- ACL: bulk IN (reassemble by the 4-byte ACL header length) and bulk OUT with
  `.flags.force_short_xfer = 1` (ZLP; `LB/sys/dev/usb/usbdi.h`).
- SCO: isochronous endpoints of interface 1 (alternate settings); not needed for A2DP: skip in M2d.
- BTstack side: implement `hci_transport_t` (`open`, `close`, `register_packet_handler`,
  `can_send_packet_now`, `send_packet` for packet types command 0x01 / ACL 0x02 / SCO 0x03, events 0x04
  upward); run the USB callbacks into BTstack's run loop through a queue (BTstack is single-threaded).
- Order: Wi-Fi loader (3.1) first; attach the HCI driver only on the `8d81` enumeration; start BTstack
  with `HCI_Reset`. Detach/reattach together with the Wi-Fi function.

## 8. Coexistence settings used by the current firmware

- BT mode `btmode = 5` (`AICBT_BTMODE_BT_ONLY_COANT`, "bt only mode with no external switch", shared
  antenna), `btport = MB`, BT TX power level word `0x6F2F` (`LF/aicbluetooth.c` `AICBT_BTMODE_DEFAULT_8800d80`,
  `aicbt_info[]`, written into the `MODE` patch-table record by `aicbt_patch_table_load`, 3.1/5).
  `btmode 1` (Wi-Fi/BT combo) was A/B tested on the Pluto on 2026-10-21 and rejected: AirPods paging
  timed out every time and Wi-Fi lost 15-20 % while paging; stock 5 gave A2DP + Wi-Fi at 14.6 ms avg,
  0 % loss (project record "pluto-aic-bt-coex"). Keep 5.
- Wi-Fi side: `MM_SET_COEX_REQ {bt_on=1, disable_coexnull=0, enable_nullcts=1, periodic=0, ts_set=0}`
  at the first interface start (`FD/rwnx_main.c:rwnx_open`, `FD/rwnx_msg_tx.c:rwnx_send_coex_req`, `CONFIG_COEX=y`).
- AFH from the host (`PR/firmware/br-external/board/nano/rootfs-overlay/usr/sbin/bt-coex`, started by
  `S42bt-coex`): BR/EDR `HCI Set AFH Host Channel Classification` (OGF 0x03, OCF 0x003F, 10-byte map,
  79 channels at 2402+k MHz) and LE `HCI LE Set Host Channel Classification` (OGF 0x08, OCF 0x0014,
  5-byte map, 37 data channels); channels within Wi-Fi center +- half width +- 1 MHz are marked bad;
  re-applied at every Wi-Fi channel change and every 30 s (adapters forget it at reset). Port as a small
  BTstack task fed by the Wi-Fi driver's channel (`ni_chan`) changes.
- A2DP stream forced to mono, low SBC quality to save airtime (`S41bluealsa` defaults; project record).

## 9. Effort and staged implementation

Estimates for one engineer who knows libbsd USB/net80211, clean-room implementation:

| Part | Effort |
|---|---|
| USB transport, IN parser, command manager (sync request/confirm, indications, timeouts, reset recovery) | 4-5 d |
| D80 loader (patch-table parser, uploads, start-app, re-enumeration handling) | 2-3 d |
| D80 application init (userconfig parser, TX-power tables, RF calib, MAC, ME config) | 3 d |
| net80211 attach, vap, scan offload, connect/disconnect state machine | 5 d |
| Keys, control port, wpa_supplicant WPA2-PSK end to end | 3-4 d |
| Data path (TX de-encap + descriptor, RX header -> rx_stats, reorder via `ampdu_rx_start_ext`, DATA_CFM) | 4-5 d |
| Country/regdomain, TX power, stats, robustness (unplug, fw crash, reset) | 3 d |
| DC bring-up (calibration tables per licence decision, DPD calib, patch table, ROM start) | 4-6 d |
| BT H2 transport for BTstack + ZLP + AFH task (A2DP profile work not included) | 4-6 d |
| **Total** | **~7-9 weeks** |

Order and checkpoints (each one testable on the Pluto console):

0. **Descriptors**: probe-only driver logs interfaces/endpoints of `8d80`, `8d81`, `368b:8d81`, `88dc`
   after the existing storage-mode switch (`PR/rtos/libbsd/aic_modeswitch.c`). Checkpoint: table filled in here.
1. **Transport**: `DBG_MEM_READ(0x40500000)` on `8d80` and `88dc`. Checkpoint: `chip_id`, `chip_mcu_id`
   (expect 1), DC `chip_sub_id` printed; equal to the Linux log values.
2. **D80 loader**: 3.1. Checkpoint: device detaches, `8d81` attaches within seconds; a stale `8d81` at
   boot is rebooted back to `8d80`.
3. **D80 init**: 3.2. Checkpoint: firmware version string, MAC equal to Linux `wlan0`, `is_5g_support=1`.
4. **Scan**: vap + `ifconfig wlan0 up scan`. Checkpoint: same BSS list/RSSI as Linux `iw scan`.
5. **Open association**: open AP or test SSID, dhcpcd. Checkpoint: DHCP lease, ping < 10 ms on 5 GHz.
6. **WPA2-PSK**: wpa_supplicant (bsd driver). Checkpoint: 4-way handshake completes, keys installed,
   control port open, DHCP over WPA2; reconnect after AP reboot; `ifconfig country` changes channels.
7. **Throughput/robustness**: iperf TX/RX both bands; unplug/replug; forced command timeout -> reset path.
   Checkpoint: >= 30 Mbit/s 5 GHz, no leak after 20 replug cycles.
8. **DC**: 3.3 after the licence decision on the tables. Checkpoint: steps 4-6 on the DC stick.
9. **Bluetooth**: H2 transport + BTstack. Checkpoint: `HCI_Reset`, `Read_BD_ADDR` equals Linux `hciconfig`;
   inquiry finds headphones; AFH map accepted (status 00); later A2DP (M2d).

Risks: H-variant D80 firmware missing (3.1/6); `chip_mcu_id=0` units need the `main` branch firmware
and possibly different message enums; RX header bit-field positions must be confirmed against live
frames (stage 3/4); BT depends on the Wi-Fi loader and resets with it.
