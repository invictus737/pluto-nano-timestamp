#!/usr/bin/env python3
"""Pluto additions to an unpacked rtems-libbsd tree (idempotent).

1. libbsd.py: compile the Zynq EHCI glue, the AIC8800 mode switch and the AIC8800 USB/ifnet
   attachment (RTEMS-side sources, next to the existing ehci_mpc83xx glue in the always-on
   "rtems" module).
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
    for src in ("'sys/dev/usb/wlan/if_aicwf_usb.c',", "'sys/dev/usb/aic_modeswitch.c',",
                "'sys/dev/usb/controller/ehci_zynq.c',"):
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

usb_error_t
usb_msc_cmd_in(struct usb_device *udev, uint8_t iface_index,
    const void *cdb, size_t cdb_len, void *data, size_t data_len)
{
	struct bbb_transfer *sc;
	usb_error_t err;

	if (cdb_len == 0 || cdb_len > CBWCDBLENGTH || data_len == 0)
		return (USB_ERR_INVAL);
	sc = bbb_attach(udev, iface_index, UICLASS_MASS);
	if (sc == NULL)
		return (USB_ERR_INVAL);
	err = bbb_command_start(sc, DIR_IN, 0, data, data_len,
	    __DECONST(void *, cdb), cdb_len, USB_MS_HZ);
	bbb_detach(sc);
	return (err);
}
"""


def msctest_c(s):
    return s if "usb_msc_cmd_in" in s else s + EJECT_CDB


def msctest_h(s):
    if "usb_msc_eject_cdb" in s:
        return s
    anchor = "usb_error_t usb_msc_auto_quirk("
    assert s.count(anchor) == 1
    proto = ("usb_error_t usb_msc_eject_cdb(struct usb_device *udev,\n"
             "\t    uint8_t iface_index, const void *cdb, size_t cdb_len);\n"
             "usb_error_t usb_msc_cmd_in(struct usb_device *udev, uint8_t iface_index,\n"
             "\t    const void *cdb, size_t cdb_len, void *data, size_t data_len);\n")
    return s.replace(anchor, proto + anchor)


QUIRK_MARK = "/* AIC8800 driver CD (Pluto RTOS) */"


def quirk_c(s):
    """AIC8800 driver-CD IDs: no MSC auto-quirk probe (its GET_MAX_LUN wedges the DC stick before
    the mode switch can run) and no umass, as usb-storage US_FL_IGNORE_DEVICE in the v0.39 image."""
    if QUIRK_MARK in s:
        return s
    anchor = "static struct usb_quirk_entry usb_quirks[USB_DEV_QUIRKS_MAX] = {\n"
    assert s.count(anchor) == 1, "anchor not unique in usb_quirk.c"
    rows = "\t" + QUIRK_MARK + "\n"
    for pid in (0x5721, 0x5722, 0x5723, 0x5724, 0x5725, 0x5726, 0x5727, 0x572a, 0x572c, 0x572f):
        rows += ("\tUSB_QUIRK_VP(0xa69c, 0x%04x, 0x0000, 0xffff, UQ_MSC_IGNORE, UQ_MSC_NO_GETMAXLUN,\n"
                 "\t    UQ_MSC_NO_INQUIRY, UQ_MSC_NO_TEST_UNIT_READY, UQ_MSC_NO_SYNC_CACHE),\n" % pid)
    rows += ("\tUSB_QUIRK_VP(0x1111, 0x1111, 0x0000, 0xffff, UQ_MSC_NO_GETMAXLUN, UQ_MSC_NO_INQUIRY,\n"
             "\t    UQ_MSC_NO_TEST_UNIT_READY, UQ_MSC_NO_SYNC_CACHE),\n")
    return s.replace(anchor, anchor + rows)


DIAG_MARK = "/* Pluto RTOS diag */"


def diag_proto_c(s):
    """trace: the parent up/down task runs (net80211 -> driver ic_parent)"""
    if DIAG_MARK in s:
        return s
    old = "\tstruct ieee80211com *ic = arg;\n\n\tic->ic_parent(ic);\n"
    assert s.count(old) == 1, "parent_updown anchor"
    return s.replace(old, "\tstruct ieee80211com *ic = arg;\n\n\tprintf(\"wlan diag: %s parent task, %d vap(s) running\\n\", ic->ic_name, ic->ic_nrunning); " + DIAG_MARK + "\n\tic->ic_parent(ic);\n")


def diag_ioctl_c(s):
    """trace: SIOCSIFFLAGS on a vap (state, running count)"""
    if DIAG_MARK in s:
        return s
    old = "\t\tif (ifp->if_flags & IFF_UP) {\n\t\t\t/*\n\t\t\t * Bring ourself up unless we're already operational."
    assert s.count(old) == 1, "SIOCSIFFLAGS anchor"
    return s.replace(old, "\t\tprintf(\"wlan diag: %s flags 0x%x state %d running %d\\n\", ifp->if_xname, (unsigned)ifp->if_flags, (int)vap->iv_state, ic->ic_nrunning); " + DIAG_MARK + "\n" + old)


def diag_rtwn_c(s):
    """trace: rtwn_init entry and its error"""
    if DIAG_MARK in s:
        return s
    old = "\tRTWN_LOCK(sc);\n\tif (sc->sc_flags & RTWN_RUNNING) {\n\t\tRTWN_UNLOCK(sc);\n\t\treturn (0);\n\t}\n\tsc->sc_flags |= RTWN_STARTED;"
    assert s.count(old) == 1, "rtwn_init anchor"
    s = s.replace(old, old + "\n\tdevice_printf(sc->sc_dev, \"diag: init, flags 0x%x\\n\", sc->sc_flags); " + DIAG_MARK)
    old = "fail:\n\tRTWN_UNLOCK(sc);\n\n\treturn (error);\n}"
    assert s.count(old) == 1, "rtwn_init fail anchor"
    return s.replace(old, "fail:\n\tif (error != 0)\n\t\tdevice_printf(sc->sc_dev, \"diag: init failed, error %d\\n\", error);\n\tRTWN_UNLOCK(sc);\n\n\treturn (error);\n}")


POLL_MARK = "/* Pluto RTOS: periodic transfer poll */"


def ehci_c(s):
    """ehci_zynq_poll(): complete finished transfers without an interrupt (the Zynq glue calls it
    from a timer); returns how many it completed, so lost completion interrupts become visible."""
    if POLL_MARK in s:
        return s
    return s + """
""" + POLL_MARK + """
int
ehci_zynq_poll(ehci_softc_t *sc)
{
	struct usb_xfer *xfer;
	int done = 0;

	USB_BUS_LOCK(&sc->sc_bus);
repeat:
	TAILQ_FOREACH(xfer, &sc->sc_bus.intr_q.head, wait_entry) {
		if (ehci_check_transfer(xfer)) {
			done++;
			goto repeat;
		}
	}
	USB_BUS_UNLOCK(&sc->sc_bus);
	return (done);
}
"""


edit("libbsd.py", libbsd_py)
edit("freebsd/sys/dev/usb/usb_msctest.c", msctest_c)
edit("freebsd/sys/dev/usb/usb_msctest.h", msctest_h)
edit("freebsd/sys/dev/usb/quirk/usb_quirk.c", quirk_c)
edit("freebsd/sys/net80211/ieee80211_proto.c", diag_proto_c)
edit("freebsd/sys/net80211/ieee80211_ioctl.c", diag_ioctl_c)
edit("freebsd/sys/dev/rtwn/if_rtwn.c", diag_rtwn_c)
edit("freebsd/sys/dev/usb/controller/ehci.c", ehci_c)
print("libbsd patched for the Pluto")
