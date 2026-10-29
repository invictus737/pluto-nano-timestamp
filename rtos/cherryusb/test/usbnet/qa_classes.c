/*
 * QA test of the CHERRY_USBNET class table (net/usbnet_classes.c): which class driver each
 * interface of the AIC8800 Wi-Fi sticks (D80, DC, storage mode) and of the USB Ethernet
 * adapters binds to, with CherryUSB v1.6.1's first-match rule (usbh_core.c
 * usbh_find_class_driver, copied below: it is static there). The AIC adapter must keep every
 * a69c interface; the Ethernet entries must take only their own.
 *
 * Built by qa_run.sh against the real table; the 32-bit-only size assertion of that file is
 * neutralised on a 64-bit host and the table is walked by its declared count.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "usbh_core.h"

const struct usbh_class_driver hub_class_driver = { .driver_name = "hub" };
const struct usbh_class_driver aicwf_cherry_class_driver = { .driver_name = "aic8800" };
const struct usbh_class_driver rtl8152_class_driver = { .driver_name = "rtl8152" };
const struct usbh_class_driver asix_class_driver = { .driver_name = "asix" };
const struct usbh_class_driver cdc_ecm_class_driver = { .driver_name = "cdc_ecm" };
const struct usbh_class_driver cdc_ncm_class_driver = { .driver_name = "cdc_ncm" };

extern const struct usbh_class_info nexus_usbh_class_table[7];

/* usbh_core.c v1.6.1 usbh_find_class_driver */
static const char *find(uint8_t class, uint8_t subclass, uint8_t protocol, uint8_t intf, uint16_t vid, uint16_t pid)
{
    for (const struct usbh_class_info *index = nexus_usbh_class_table; index < nexus_usbh_class_table + 7; index++) {
        if ((index->match_flags & USB_CLASS_MATCH_INTF_CLASS) && !(index->bInterfaceClass == class))
            continue;
        if ((index->match_flags & USB_CLASS_MATCH_INTF_SUBCLASS) && !(index->bInterfaceSubClass == subclass))
            continue;
        if ((index->match_flags & USB_CLASS_MATCH_INTF_PROTOCOL) && !(index->bInterfaceProtocol == protocol))
            continue;
        if ((index->match_flags & USB_CLASS_MATCH_INTF_NUM) && !(index->bInterfaceNumber == intf))
            continue;
        if (index->match_flags & USB_CLASS_MATCH_VID_PID && index->id_table) {
            uint32_t i;
            for (i = 0; index->id_table[i][0]; i++) {
                if (index->id_table[i][0] == vid && index->id_table[i][1] == pid)
                    break;
            }
            if (!index->id_table[i][0])
                continue;
        }
        return index->class_driver->driver_name;
    }
    return "(none)";
}

static int g_fail;
static void expect(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("  FAIL %s: %s, expected %s\n", what, got, want);
        g_fail++;
    } else {
        printf("  ok   %-48s -> %s\n", what, got);
    }
}

int main(void)
{
    printf("qa: class table binding\n");
    /* AIC8800 D80 (a69c:8d80) / DC (a69c:88dc, 88de) Wi-Fi + BT, storage mode (a69c:5721) */
    expect("AIC D80 wlan if0 ff/ff/ff", find(0xff, 0xff, 0xff, 0, 0xa69c, 0x8d80), "aic8800");
    expect("AIC D80 bt if1 e0/01/01", find(0xe0, 0x01, 0x01, 1, 0xa69c, 0x8d80), "aic8800");
    expect("AIC DC wlan if0 ff/ff/ff", find(0xff, 0xff, 0xff, 0, 0xa69c, 0x88dc), "aic8800");
    expect("AIC DC if2 ff/00/00", find(0xff, 0x00, 0x00, 2, 0xa69c, 0x88de), "aic8800");
    expect("AIC storage mode 08/06/50", find(0x08, 0x06, 0x50, 0, 0xa69c, 0x5721), "aic8800");
    /* Ethernet */
    expect("RTL8152 vendor ff/ff/00", find(0xff, 0xff, 0x00, 0, 0x0bda, 0x8152), "rtl8152");
    expect("RTL8153 vendor cfg ff/ff/00 (not ours)", find(0xff, 0xff, 0x00, 0, 0x0bda, 0x8153), "aic8800");
    expect("RTL8153 ECM cfg 02/06/00", find(0x02, 0x06, 0x00, 0, 0x0bda, 0x8153), "cdc_ecm");
    expect("RTL8153 ECM data 0a/00/00", find(0x0a, 0x00, 0x00, 1, 0x0bda, 0x8153), "cdc_data");
    expect("RTL8156 NCM 02/0d/00", find(0x02, 0x0d, 0x00, 0, 0x0bda, 0x8156), "cdc_ncm");
    expect("AX88772B ff/ff/00", find(0xff, 0xff, 0x00, 0, 0x0b95, 0x772b), "asix");
    expect("AX88179 ff/ff/00 (not ours)", find(0xff, 0xff, 0x00, 0, 0x0b95, 0x1790), "aic8800");
    expect("hub 09/00/00", find(0x09, 0x00, 0x00, 0, 0x05e3, 0x0610), "hub");
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
