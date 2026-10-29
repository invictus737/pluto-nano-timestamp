/*
 * GET /metrics "usb" member for USB_STACK=cherry images: the ehci-nxp host controller counters
 * (the same numbers `usbh` prints). Built with the CherryUSB include path (rtos/app/Makefile);
 * metrics.c references metrics_usb_json() weakly, so libbsd USB images report "usb": null.
 * Plain reads of free-running uint32 counters, no lock: a value may be one update old.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <stdio.h>

#include "usbh_core.h"
#include "usb_hc_ehci.h"

#include "metrics.h"

void metrics_usb_json(jw_t *w)
{
  jw_obj(w, "usb");
  jw_str(w, "stack", "cherry");
#ifdef USB_HC_EHCI_NXP
  {
    const struct ehci_nxp_stats *ns = &g_ehci_nxp[0].stats;
    uint32_t open = 0;
    for (uint32_t i = 0; i < CONFIG_USB_EHCI_QH_NUM; i++) {
      open += g_ehci_nxp[0].pipes[i].inuse;
    }
    jw_str(w, "hcd", "ehci-nxp");
    jw_uint(w, "pipes_open", open);
    jw_uint(w, "pipes_opened", ns->pipes_opened);
    jw_uint(w, "pipes_closed", ns->pipes_closed);
    jw_uint(w, "no_pipe", ns->no_pipe);
    jw_uint(w, "qtd_free", g_ehci_nxp[0].qtd_free_count);
    jw_uint(w, "submits", ns->submits);
    jw_uint(w, "completions", ns->completions);
    jw_uint(w, "errors", ns->errors);
    jw_uint(w, "stalls", ns->stalls);
    jw_uint(w, "halted_restarts", ns->halted_restarts);
    jw_uint(w, "xacterr_restarts", ns->xacterr_restarts);
    jw_uint(w, "kills", ns->kills);
    jw_uint(w, "kill_waits", ns->kill_waits);
    jw_uint(w, "kill_busy", ns->kill_busy);
    jw_uint(w, "handshake_misses", ns->handshake_timeouts);
    jw_uint(w, "max_handshake_us", ns->max_handshake_us);
    jw_uint(w, "async_stops", ns->async_stops);
    jw_uint(w, "periodic_stops", ns->periodic_stops);
    jw_uint(w, "deferred", ns->deferred);
    jw_uint(w, "orphans", ns->orphans);
    jw_uint(w, "no_qtd", ns->no_qtd);
    jw_uint(w, "no_xfer", ns->no_xfer);
    jw_uint(w, "resets", ns->recoveries);
    jw_uint(w, "reset_failures", ns->recover_failures);
    jw_uint(w, "last_reset_ms", ns->last_recover_ms);
    jw_uint(w, "board_resets", ns->board_resets);
    jw_uint(w, "hse", ns->hse);
    jw_uint(w, "halted", ns->halted_seen);
    jw_uint(w, "wedges", ns->wedges);
    jw_uint(w, "stuck_cancels", ns->stuck_cancels);
    jw_uint(w, "stuck_pending", ns->stuck_pending);
    jw_uint(w, "stale_refused", ns->stale_refused);
    jw_uint(w, "dma_halts", ns->dma_halts);
    jw_bool(w, "gate", g_ehci_nxp[0].gate != 0);
  }
#else
  jw_str(w, "hcd", "cherry-ehci");
#endif
  {
    const struct usbh_ehci_iaa_stats *st = &g_ehci_iaa_stats[0];
    jw_uint(w, "async_kicks", st->async_kicks);
    jw_uint(w, "async_kick_no_handshake", st->async_kick_no_handshake);
    jw_uint(w, "fatal", st->fatal);
    jw_uint(w, "ok", st->ok_count);
  }
  jw_uint(w, "xacterr_retries", g_ehci_xacterr_retries);
  jw_obj_end(w);
}
