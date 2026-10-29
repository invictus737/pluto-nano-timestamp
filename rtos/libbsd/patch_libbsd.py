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


SCHED_MARK = "/* Pluto RTOS: schedules stay on */"


def ehci_c(s):
    """1. ehci_set_hw_power toggles USBCMD ASE/PSE with the bus power state without waiting for
    USBSTS AS/PS to follow (EHCI 4.8: software must not change the enable until the status
    matches; Linux handshakes). The ChipIdea core on the Zynq then stays in the transition (seen:
    ASE=0 with AS=1 and an unacknowledged IAAD) and new transfers are never fetched: every driver
    times out. Keep both schedules enabled.
    2. ehci_zynq_poll(): complete finished transfers without an interrupt (the Zynq glue calls it
    from a timer); returns how many it completed."""
    if SCHED_MARK not in s:
        old = """	temp &= ~(EHCI_CMD_ASE | EHCI_CMD_PSE);

	if (flags & (USB_HW_POWER_CONTROL |"""
        assert s.count(old) == 1, "ehci_set_hw_power anchor"
        s = s.replace(old, """	""" + SCHED_MARK + """
	temp |= EHCI_CMD_ASE | EHCI_CMD_PSE;
	if (0 && flags & (USB_HW_POWER_CONTROL |""")
    if POLL_MARK in s:
        return s
    return s + """
""" + POLL_MARK + """
unsigned ehci_zynq_ring_repairs;\nstatic unsigned ehci_zynq_unlinked_n;

/*
 * The async ring as the controller walks it must match the software list (head -> next ... ->
 * sc_async_p_last -> head). Seen on the Pluto: the head QH linked to itself while the live QHs
 * hung off a removed one, so the controller never fetched them and every transfer timed out.
 * Re-link from the software list; splice a chain left behind a removed QH back after the last
 * reachable one. Bus lock held.
 */
static void
ehci_zynq_async_check(ehci_softc_t *sc)
{
	struct usb_page_search buf_res;
	ehci_qh_t *head, *q, *z;
	int n;

	usbd_get_page(&sc->sc_hw.async_start_pc, 0, &buf_res);
	head = buf_res.buffer;
	q = head;
	for (n = 0; q->next != NULL && n < 128; ++n) {
		if (q->qh_link != q->next->qh_self) {
			q->qh_link = q->next->qh_self;
			usb_pc_cpu_flush(q->page_cache);
			ehci_zynq_ring_repairs++;
		}
		q = q->next;
	}
	if (q != sc->sc_async_p_last && sc->sc_async_p_last != NULL) {
		/* the tail is not reachable: find the detached root behind it */
		for (z = sc->sc_async_p_last, n = 0; z->prev != NULL && z->prev != q && n < 128; ++n)
			z = z->prev;
		if (z->prev == NULL && z != head) {
			ehci_qh_t *first = z->next;
			z->next = NULL;
			if (first != NULL) {
				q->next = first;
				first->prev = q;
				q->qh_link = first->qh_self;
				usb_pc_cpu_flush(q->page_cache);
				for (n = 0; q->next != NULL && n < 128; ++n)
					q = q->next;
			}
			ehci_zynq_ring_repairs++;
		}
		sc->sc_async_p_last = q;
	}
	if (q->qh_link != head->qh_self) {
		q->qh_link = head->qh_self;
		usb_pc_cpu_flush(q->page_cache);
		ehci_zynq_ring_repairs++;
	}
	/* every pending control/bulk transfer must have its QH on the list */
	{
		struct usb_xfer *xfer;
		TAILQ_FOREACH(xfer, &sc->sc_bus.intr_q.head, wait_entry) {
			const struct usb_pipe_methods *m = xfer->endpoint->methods;
			ehci_qh_t *qh;
			int on;
			if (m != &ehci_device_ctrl_methods && m != &ehci_device_bulk_methods)
				continue;
			qh = xfer->qh_start[xfer->flags_int.curr_dma_set];
			if (qh == NULL)
				continue;
			for (on = 0, z = head; z != NULL && !on; z = z->next)
				on = (z == qh);
			if (!on) {
				if (ehci_zynq_unlinked_n++ < 20)
					printf("ehci: pending xfer addr %d ep 0x%02x has its QH off the list "
					    "(prev %p next %p), relinked\\n", xfer->address, xfer->endpointno,
					    qh->prev, qh->next);
				qh->prev = NULL;
				qh->next = NULL;
				EHCI_APPEND_QH(qh, sc->sc_async_p_last);
				ehci_zynq_ring_repairs++;
			}
		}
	}
}

int
ehci_zynq_poll(ehci_softc_t *sc)
{
	struct usb_xfer *xfer;
	int done = 0;

	USB_BUS_LOCK(&sc->sc_bus);
	ehci_zynq_async_check(sc);
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


NC_MARK = "/* Pluto RTOS: drain to the device */"


def busdma_c(s):
    """The non-cacheable DMA area is mapped Normal non-cacheable (unaligned CPU accesses allowed).
    Such writes can sit in the CPU and L2C-310 store buffers (erratum 769419: visibility needs an
    explicit Cache Sync), so a descriptor update may never reach the USB controller and the
    transfer times out. Before the device reads: DSB + L2 Cache Sync (a 1-line range flush ends
    with both)."""
    if NC_MARK in s:
        return s
    old = """	if (begin >= (uintptr_t)bsp_section_nocache_begin &&
	    end <= (uintptr_t)bsp_section_nocachenoload_end) {
		return;
	}"""
    assert s.count(old) == 1, "bus_dmamap_sync nocache anchor"
    new = """	if (begin >= (uintptr_t)bsp_section_nocache_begin &&
	    end <= (uintptr_t)bsp_section_nocachenoload_end) {
		""" + NC_MARK + """
		if ((op & (BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD)) != 0)
			rtems_cache_flush_multiple_data_lines((void *) begin, 1);
		return;
	}"""
    return s.replace(old, new)


TRACE_MARK = "/* Pluto RTOS: transfer trace */"


def ehci_trace_c(s):
    """Errors and timeouts at the qTD/QH level, printed (bounded) without USB_DEBUG."""
    if TRACE_MARK in s:
        return s
    old = """#ifdef USB_DEBUG
	if (status & EHCI_QTD_STATERRS) {
		DPRINTFN(11, "error, addr=%d, endpt=0x%02x, frame=0x%02x\""""
    assert s.count(old) == 1, "done_sub error anchor"
    s = s.replace(old, """	""" + TRACE_MARK + """
	if ((status & EHCI_QTD_STATERRS) != 0) {
		static int n;
		if (n++ < 60)
			printf("ehci: xfer error addr %d ep 0x%02x len %u status 0x%08x cerr %d%s%s%s%s%s\\n",
			    xfer->address, xfer->endpointno, (unsigned)xfer->sumlen, (unsigned)status,
			    (int)EHCI_QTD_GET_CERR(status),
			    (status & EHCI_QTD_HALTED) ? " HALTED" : "",
			    (status & EHCI_QTD_BUFERR) ? " BUFERR" : "",
			    (status & EHCI_QTD_BABBLE) ? " BABBLE" : "",
			    (status & EHCI_QTD_XACTERR) ? " XACTERR" : "",
			    (status & EHCI_QTD_MISSEDMICRO) ? " MISSED" : "");
	}
#ifdef USB_DEBUG
	if (status & EHCI_QTD_STATERRS) {
		DPRINTFN(11, "error, addr=%d, endpt=0x%02x, frame=0x%02x\"""")
    old = """	DPRINTF("xfer=%p\\n", xfer);

	USB_BUS_LOCK_ASSERT(xfer->xroot->bus, MA_OWNED);

	/* transfer is transferred */
	ehci_device_done(xfer, USB_ERR_TIMEOUT);"""
    assert s.count(old) == 1, "ehci_timeout anchor"
    return s.replace(old, """	DPRINTF("xfer=%p\\n", xfer);

	USB_BUS_LOCK_ASSERT(xfer->xroot->bus, MA_OWNED);
	{
		static int n;
		ehci_softc_t *tsc = EHCI_BUS2SC(xfer->xroot->bus);
		ehci_qh_t *qh = xfer->qh_start[xfer->flags_int.curr_dma_set];
		ehci_qtd_t *td = xfer->td_transfer_cache;
		if (n++ < 40 && qh != NULL) {
			usb_pc_cpu_invalidate(qh->page_cache);
			if (td != NULL)
				usb_pc_cpu_invalidate(td->page_cache);
			printf("ehci: timeout addr %d ep 0x%02x len %u qh endp 0x%08x cur 0x%08x "
			    "overlay 0x%08x td 0x%08x/0x%08x cmd 0x%08x sts 0x%08x\\n",
			    xfer->address, xfer->endpointno, (unsigned)xfer->sumlen,
			    (unsigned)hc32toh(tsc, qh->qh_endp), (unsigned)hc32toh(tsc, qh->qh_curqtd),
			    (unsigned)hc32toh(tsc, qh->qh_qtd.qtd_status),
			    td != NULL ? (unsigned)td->qtd_self : 0u,
			    td != NULL ? (unsigned)hc32toh(tsc, td->qtd_status) : 0u,
			    (unsigned)EOREAD4(tsc, EHCI_USBCMD), (unsigned)EOREAD4(tsc, EHCI_USBSTS));
		}
	}

	/* transfer is transferred */
	ehci_device_done(xfer, USB_ERR_TIMEOUT);
	{
		extern void zynq_ehci_kick_async(void);
		/* a timed-out QH may be the one the controller is parked on */
		zynq_ehci_kick_async();
	}""")


PM_MARK = "/* Pluto RTOS: no USB suspend */"


def usb_hub_c(s):
    """USB selective suspend off: hubs attach in USB_POWER_MODE_SAVE and get suspended by the
    power daemon; a transfer to a self-suspended device has its QH kept off the EHCI schedule
    (ehci_setup_standard_chain) and times out (seen: hub and rtwn control QHs never fetched).
    The Pluto is always powered: every device stays ON."""
    if PM_MARK in s:
        return s
    old = """	/* filter input argument */
	if ((power_mode != USB_POWER_MODE_ON) &&
	    (power_mode != USB_POWER_MODE_OFF))
		power_mode = USB_POWER_MODE_SAVE;"""
    assert s.count(old) == 1, "usbd_set_power_mode anchor"
    return s.replace(old, """	""" + PM_MARK + """
	if (power_mode != USB_POWER_MODE_OFF)
		power_mode = USB_POWER_MODE_ON;""")


edit("libbsd.py", libbsd_py)
edit("freebsd/sys/dev/usb/usb_msctest.c", msctest_c)
edit("freebsd/sys/dev/usb/usb_msctest.h", msctest_h)
edit("freebsd/sys/dev/usb/quirk/usb_quirk.c", quirk_c)
edit("freebsd/sys/net80211/ieee80211_proto.c", diag_proto_c)
edit("freebsd/sys/net80211/ieee80211_ioctl.c", diag_ioctl_c)
edit("freebsd/sys/dev/rtwn/if_rtwn.c", diag_rtwn_c)
edit("freebsd/sys/dev/usb/controller/ehci.c", ehci_c)
edit("freebsd/sys/dev/usb/controller/ehci.c", ehci_trace_c)
edit("rtemsbsd/rtems/rtems-kernel-bus-dma.c", busdma_c)
edit("freebsd/sys/dev/usb/usb_hub.c", usb_hub_c)
print("libbsd patched for the Pluto")
