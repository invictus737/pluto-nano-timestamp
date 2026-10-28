#!/usr/bin/env python3
"""Pluto additions to an unpacked rtems-libbsd tree (idempotent).

1. libbsd.py: compile the Zynq EHCI glue and the AIC8800 mode switch (RTEMS-side sources,
   next to the existing ehci_mpc83xx glue in the always-on "rtems" module).
2. usb_msctest: usb_msc_eject_cdb() sends one arbitrary CDB without a data phase (the
   AIC8800 switch commands are not among FreeBSD's fixed eject methods).
"""
import os
import sys

lb = sys.argv[1]


def edit(rel, fn):
    path = os.path.join(lb, rel)
    with open(path) as f:
        old = f.read()
    new = fn(old)
    if new != old:
        with open(path, "w") as f:
            f.write(new)


def libbsd_py(s):
    anchor = "'sys/dev/usb/controller/ehci_mpc83xx.c',"
    assert s.count(anchor) == 1, "anchor not unique in libbsd.py"
    i = s.index(anchor)
    indent = s[s.rindex("\n", 0, i) + 1:i]
    for src in ("'sys/dev/usb/aic_modeswitch.c',", "'sys/dev/usb/controller/ehci_zynq.c',"):
        if src not in s:
            s = s.replace(anchor, anchor + "\n" + indent + src)
    return s


EJECT_CDB = """
usb_error_t
usb_msc_eject_cdb(struct usb_device *udev, uint8_t iface_index,
    const void *cdb, size_t cdb_len)
{
	struct bbb_transfer *sc;
	usb_error_t err;

	if (cdb_len == 0 || cdb_len > CBWCDBLENGTH)
		return (USB_ERR_INVAL);
	sc = bbb_attach(udev, iface_index, UICLASS_MASS);
	if (sc == NULL)
		return (USB_ERR_INVAL);
	err = bbb_command_start(sc, DIR_NONE, 0, NULL, 0,
	    __DECONST(void *, cdb), cdb_len, USB_MS_HZ);
	bbb_detach(sc);
	return (err);
}
"""


def msctest_c(s):
    return s if "usb_msc_eject_cdb" in s else s + EJECT_CDB


def msctest_h(s):
    if "usb_msc_eject_cdb" in s:
        return s
    anchor = "usb_error_t usb_msc_auto_quirk("
    assert s.count(anchor) == 1
    proto = ("usb_error_t usb_msc_eject_cdb(struct usb_device *udev,\n"
             "\t    uint8_t iface_index, const void *cdb, size_t cdb_len);\n")
    return s.replace(anchor, proto + anchor)


edit("libbsd.py", libbsd_py)
edit("freebsd/sys/dev/usb/usb_msctest.c", msctest_c)
edit("freebsd/sys/dev/usb/usb_msctest.h", msctest_h)
print("libbsd patched for the Pluto")
