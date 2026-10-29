/*
 * CherryUSB host configuration for the Pluto Nano RTEMS image (USB_STACK=cherry).
 *
 * One controller: Zynq-7000 USB0, a ChipIdea (Synopsys/ARC TDI) EHCI core with an integrated
 * transaction translator and a ULPI USB3320 PHY, one root port, high speed. Devices: the AIC8800
 * Wi-Fi/BT stick (vendor + Bluetooth interfaces), behind at most a couple of hubs.
 *
 * Memory model: QH/qTD pools, the frame list and every CherryUSB-internal transfer buffer live
 * in the BSP non-cacheable NOLOAD area (.bsp_noload_nocache, zeroed by the glue; an initialised
 * .bsp_nocache would put the 512 MiB gap into the binary; Normal non-cacheable on this BSP); the OSAL drains
 * the L2C-310 store buffer when it leaves the outermost critical section (osal/usb_osal_rtems.c).
 * Class-driver buffers are cacheable: CONFIG_USB_DCACHE_ENABLE makes the EHCI port clean and
 * invalidate them before a transfer, aic/aicwf_cherry.c invalidates IN buffers after it and
 * bounces buffers that are not cache-line aligned.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NEXUS_CHERRYUSB_CONFIG_H
#define NEXUS_CHERRYUSB_CONFIG_H

#include <stdio.h>

/* ================ common ================ */

#define CONFIG_USB_PRINTF(...) printf(__VA_ARGS__)

#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL USB_DBG_INFO
#endif

/* Cortex-A9 L1 and the L2C-310 both use 32-byte lines */
#define CONFIG_USB_DCACHE_ENABLE
#define CONFIG_USB_ALIGN_SIZE 32

#define USB_NOCACHE_RAM_SECTION __attribute__((section(".bsp_noload_nocache.cherryusb")))

/* ================ host stack ================ */

#define CONFIG_USBHOST_MAX_RHPORTS          1
#define CONFIG_USBHOST_MAX_EXTHUBS          2
#define CONFIG_USBHOST_MAX_EHPORTS          7
/* AIC8800 composites: vendor Wi-Fi interface + Bluetooth (event/ACL + isochronous alternates) */
#define CONFIG_USBHOST_MAX_INTERFACES       8
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 8
#define CONFIG_USBHOST_MAX_ENDPOINTS        8

#define CONFIG_USBHOST_MAX_SERIAL_CLASS 1
#define CONFIG_USBHOST_MAX_HID_CLASS    1
#define CONFIG_USBHOST_MAX_MSC_CLASS    1
#define CONFIG_USBHOST_MAX_AUDIO_CLASS  1
#define CONFIG_USBHOST_MAX_VIDEO_CLASS  1

#define CONFIG_USBHOST_DEV_NAMELEN 16

/*
 * Hub thread: it runs enumeration and the class drivers' connect(), and the AIC driver's
 * connect() downloads the firmware and starts the station there. Priority in the CherryUSB
 * scale (smaller = more urgent); the OSAL maps it like the AIC shim maps RT-Thread priorities
 * (POSIX 150 - prio), so 14 equals the shim's attach thread.
 */
#define CONFIG_USBHOST_PSC_PRIO      14
#define CONFIG_USBHOST_PSC_STACKSIZE (64 * 1024)

/* composite AIC configuration descriptors exceed 512 bytes with the Bluetooth alternates */
#define CONFIG_USBHOST_REQUEST_BUFFER_LEN       1024
#define CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT 2000
#define CONFIG_USBHOST_MSC_TIMEOUT              5000

/* ================ host port ================ */

#define CONFIG_USBHOST_MAX_BUS 1

/* ---------------- EHCI (ChipIdea) ---------------- */

/*
 * USB0 at 0xE0002000: capability registers at +0x100, CAPLENGTH 0x40 puts the operational
 * registers at +0x140 (USBCMD), PORTSC1 at +0x184, i.e. the standard EHCI layout with the
 * reserved words before CONFIGFLAG kept.
 */
#define CONFIG_USB_EHCI_HCCR_OFFSET     (0x100)
#define CONFIG_USB_EHCI_FRAME_LIST_SIZE 1024
/* one QH per URB in flight (aicwf_cherry.c: one URB per endpoint, or one stream QH per bulk IN) */
#define CONFIG_USB_EHCI_QH_NUM          24
#define CONFIG_USB_EHCI_QTD_NUM         96
#define CONFIG_USB_EHCI_ITD_NUM         4
/* integrated TT: no companion controller (CONFIG_USB_EHCI_WITH_OHCI stays undefined) */
/* descriptors are in non-cacheable memory: no descriptor cache maintenance */
/* no CONFIG_USB_EHCI_CONFIGFLAG, no CONFIG_USB_EHCI_ISO, no CONFIG_USB_EHCI_HCOR_RESERVED_DISABLE */
/* usbh_kill_urb() leaves PSE/ASE alone (patches/0002); remove to get the stock toggle back */
#define CONFIG_USB_EHCI_KILL_KEEP_SCHEDULES
/* restart a qTD halted by transaction errors in place, as Linux (QH_XACTERR_MAX, patches/0003) */
#define CONFIG_USB_EHCI_XACTERR_RETRIES 32
/*
 * IAA cycles (patches/0004): descriptor stores are drained to memory (DSB + L2C-310 cache
 * sync, osal/usb_osal_rtems.c) before every doorbell and schedule-enable write; a cycle that
 * got no IAA ends after 10 ms (Linux EHCI_HRTIMER_IAA_WATCHDOG).
 */
void usb_osal_dma_barrier(void);
#define CONFIG_USB_EHCI_DMA_WMB()          usb_osal_dma_barrier()
#define CONFIG_USB_EHCI_IAA_WATCHDOG_US    10000
/*
 * NAK count reload of high-speed bulk QHs (patches/0006): 0 as stock CherryUSB and every image
 * whose Wi-Fi worked (up to rtos-ci-82); 4 is Linux qh_make() (EHCI_TUNE_RL_HS), in the
 * driver since patches/0005 (build 84) and not proven on this core.
 */
#define CONFIG_USB_EHCI_BULK_RL_HS         0
/*
 * Asynchronous engine watchdog (patches/0006): a QH whose next qTD stays active but never
 * fetched, or a doorbell unanswered, for this long restarts the asynchronous schedule (ASE
 * off/on with the ASS handshake), as the stock usbh_kill_urb() did by accident at every
 * cancel before patches/0004 and the libbsd stack's poll did. 0 disables it.
 */
#define CONFIG_USB_EHCI_ASYNC_KICK_US      20000

#define CONFIG_USB_HS

/* the RTEMS address space is identity mapped */
#define usb_phyaddr2ramaddr(addr) (addr)
#define usb_ramaddr2phyaddr(addr) (addr)

/* ================ Nexus glue (port/usb_glue_zynq.c) ================ */

#define NEXUS_USB0_BASE      0xE0002000U
/*
 * poll period of the lost-interrupt check (the IAA watchdog polls every 2 ms during a cycle);
 * CHERRY_HCD=nxp has no IAA: completions are interrupt-driven only, the poll is a 1 s
 * watchdog (lost interrupt, port change without interrupt), 2 ms only while a cancel/close
 * waits for a missed schedule handshake
 */
#define NEXUS_USB_POLL_MS     10
#define NEXUS_USB_NXP_POLL_MS 1000
/*
 * SCHED_FIFO priority of the "usbh_irq" handler thread, below the POSIX maximum: 18 puts it
 * above the station's SDR workers (max - 19) and below the PL DMA service threads (max - 2).
 * Its work per interrupt is a schedule scan and the completions (tens of us); a run-time
 * value in nexus_cherryusb_irq_prio (POSIX priority, 0 = this default) overrides it.
 */
#define NEXUS_USB_IRQ_PRIO_FROM_MAX 18
/*
 * Bulk IN URBs the AIC adapter keeps in the controller per endpoint (aicwf_cherry.c). 1: one
 * at a time on a QH of its own, as in every image whose Wi-Fi worked; more chains them on one
 * EHCI stream QH (patches/0005, build 84, not proven on hardware).
 */
#define NX_IN_URBS_PER_EP 1
/* CPU of every CherryUSB thread (the AIC driver threads run on CPU0 as well, AICWF_THREAD_CPU) */
#define NEXUS_USB_THREAD_CPU 0

/* ================ USB Ethernet (rtos/cherryusb/net; usbnet section, keep last) ================ */
/*
 * CherryUSB's own class drivers (class/cdc ECM/NCM, class/vendor/net RTL8152/ASIX), glued to
 * libbsd ifnets ueN by net/usbnet.c. The class buffers are USB_NOCACHE_RAM_SECTION (16 MiB
 * NOCACHE region, bsp/config.ini): about 75 KiB with the sizes below.
 * RTL8152: the chip aggregates up to rx_buf_sz = 16 KiB per transfer (rtl_ops_init); 32 KiB keeps
 * a second 16 KiB transfer for an aggregate that ends exactly on a packet boundary.
 * CDC-NCM: the driver sets no NTB input size, the device sends up to its dwNtbInMaxSize.
 * ASIX: 2048 selects AX_RX_CTL_MFB_2048 (one frame per transfer).
 */
#define CONFIG_USBHOST_RTL8152_ETH_MAX_RX_SIZE (32 * 1024)
#define CONFIG_USBHOST_RTL8152_ETH_MAX_TX_SIZE 2048
#define CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE (32 * 1024)
#define CONFIG_USBHOST_CDC_NCM_ETH_MAX_TX_SIZE 2048
#define CONFIG_USBHOST_ASIX_ETH_MAX_RX_SIZE    2048
#define CONFIG_USBHOST_ASIX_ETH_MAX_TX_SIZE    2048
/* rx threads run the libbsd input path (ether_input .. tcp_input) inline: CherryUSB scale, one
 * below the hub thread as CherryUSB's platform glue does (POSIX 135) */
#define NEXUS_USBNET_RX_PRIO       (CONFIG_USBHOST_PSC_PRIO + 1)
#define NEXUS_USBNET_RX_STACKSIZE  (32 * 1024)
/* link poll of chips whose interrupt endpoint answers every interval (RTL8152, ASIX); CDC
 * ECM/NCM only notify on change and are waited on without a period */
#define NEXUS_USBNET_LINK_POLL_MS  500

#endif /* NEXUS_CHERRYUSB_CONFIG_H */
