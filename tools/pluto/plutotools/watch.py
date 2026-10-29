"""pluto-watch: live terminal view of the board's /metrics, JSONL recorder, log follower.

  pluto-watch 192.168.1.160                     refresh every 2 s
  pluto-watch 192.168.1.160 -i 1 --record run.jsonl --log-file run.log
  pluto-watch 192.168.1.160 --once              one screen, exit 1 on a budget failure

Red flags (budget, metrics.Budget): deadline CPU (CPU1) > 60 %, other CPU > 60 % busy,
tx.late or USB error/handshake/reset counters moving, Wi-Fi disconnected, reboot, nexus2 stop.
Every `--full-every` seconds the poll asks full=1 (stack watermarks + network events): that
scans the stacks on the board, so keep it rare.
"""

import argparse
import json
import os
import sys
import time

from . import client, metrics, stats

RED, YEL, GRN, DIM, RST = "\033[31;1m", "\033[33m", "\033[32m", "\033[2m", "\033[0m"


class Painter:
    def __init__(self, color):
        self.color = color

    def c(self, code, text):
        return code + text + RST if self.color else text


def render(m, flat, prev_flat, flags, start_flat, paint, width=100):
    """The screen as a list of lines."""
    L = []
    b = m.get("build") or {}
    n2 = m.get("nexus2") or {}
    L.append("Pluto RTEMS  %s  build %s%s  nexus %s  up %s  nexus2 %s (%s)  collect %.1f ms" % (
        b.get("image", "?"), b.get("git", "?"), (" run " + b["ci_run"]) if b.get("ci_run") else "",
        (b.get("nexus_ref") or "-")[:8], _dur(m.get("uptime_s")), n2.get("state", "?"),
        n2.get("layout", "?"), m.get("collect_ms") or 0.0))
    cpu = m.get("cpu") or {}
    busy = cpu.get("busy_pct") or []
    parts = []
    for i, v in enumerate(busy):
        bad = any(f[1] == "cpu%d" % i for f in flags)
        parts.append(paint.c(RED if bad else GRN, "CPU%d %5.1f %%" % (i, v)))
    L.append("%s   over %.1f s   threads %s" % ("   ".join(parts), cpu.get("interval_s") or 0.0,
                                                 cpu.get("threads", "?")))
    th = m.get("threads") or []
    if th:
        L.append(paint.c(DIM, "  %-24s %3s %3s %5s %7s" % ("thread", "cpu", "aff", "prio", "%cpu")))
        for t in th[:10]:
            L.append("  %-24s %3s %3s %5s %7.2f" % (t["name"][:24], t["cpu"], t["aff"], t["prio"], t["pct"]))
    st = m.get("stages") or {}
    items = st.get("items") or []
    if items:
        L.append(paint.c(DIM, "  %-18s %9s %8s %8s %8s %6s %9s %9s" % (
            "stage", "mean_us", "p99_us", "max_us", "budget", "over", "us/s", "win_mean")))
        for it in items:
            name = it["name"]
            wmean, _ = metrics.stage_window(prev_flat, flat, name) if prev_flat else (None, None)
            over = it.get("over") or 0
            line = "  %-18s %9.1f %8d %8d %8d %6d %9.0f %9s" % (
                name[:18], it["mean_us"], it["p99_us"], it["max_us"], it["budget_us"], over,
                it["us_per_s"], stats.fmt(wmean))
            L.append(paint.c(YEL, line) if any(f[1] == "stage.%s.over" % name for f in flags) else line)
    if st:
        late = st.get("tx_late")
        dl = stats.delta(start_flat.get("tx_late"), late) if start_flat else None
        L.append("  tx.late %s (since watch start %s)" % (
            paint.c(RED if (dl or 0) > 0 else GRN, stats.fmt(late)), stats.fmt(dl)))
    ad = m.get("ad9363")
    if ad:
        L.append("AD9363 %s  rate %s  rx %s / tx %s MHz  gain %s dB (%s, %s)  atten %s dB  ovf %s  "
                 "discarded %s  %s C" % (
                     "open" if ad.get("open") else "closed", stats.fmt(ad.get("rate_hz"), 0),
                     _mhz(ad.get("rx_lo_hz")), _mhz(ad.get("tx_lo_hz")), stats.fmt(ad.get("rx_gain_db")),
                     ad.get("rx_agc", "-"), ad.get("ensm", "-"), stats.fmt(ad.get("tx_atten_db"), 2),
                     stats.fmt(ad.get("tx_overflow_bits")), stats.fmt(ad.get("tx_discarded")),
                     stats.fmt(ad.get("temp_c"))))
    usb = m.get("usb")
    if usb:
        keys = ("pipes_open", "submits", "errors", "handshake_misses", "resets", "board_resets", "kills",
                "stalls", "xacterr_restarts", "deferred")
        txt = "  ".join("%s %s" % (k, usb.get(k, "-")) for k in keys)
        bad = any(f[1].startswith("usb.") and f[0] == "fail" for f in flags)
        L.append("USB " + (paint.c(RED, txt) if bad else txt))
    net = m.get("net") or {}
    w = net.get("wifi")
    ifs = " ".join("%s:%s%s" % (i["name"], i.get("link", "?"), ("/" + ",".join(i["ipv4"])) if i.get("ipv4") else "")
                   for i in net.get("ifaces") or [])
    dr = net.get("default_route")
    wtxt = "-"
    if w is not None:
        wtxt = ("%s %s dBm ch %s" % (w.get("ssid"), w.get("rssi_dbm"), w.get("channel"))) if w.get("connected") \
            else paint.c(RED, "DISCONNECTED (attempts %s, err %s)" % (w.get("connect_attempts"), w.get("last_error")))
    L.append("NET %s  route %s  dns %s  wifi %s" % (
        ifs, ("%s via %s" % (dr["gw"], dr["if"])) if dr else paint.c(RED, "none"),
        ",".join(net.get("dns") or []) or "-", wtxt))
    for e in (net.get("events") or [])[-4:]:
        L.append(paint.c(DIM, "    " + e))
    heap = m.get("heap") or {}
    t = m.get("temp") or {}
    L.append("HEAP free %s  min %s  largest %s  failed %s    TEMP zynq %s C" % (
        _mib(heap.get("free")), _mib(heap.get("min_free")), _mib(heap.get("largest_free")),
        heap.get("failed_allocs", "-"), stats.fmt(t.get("zynq_c"))))
    stacks = m.get("stacks")
    if stacks:
        L.append("STACKS " + "  ".join("%s %.0f%%" % (s["name"][:14], s["pct"]) for s in stacks[:6]))
    if flags:
        for sev, _, text in flags:
            L.append(paint.c(RED if sev == "fail" else YEL, "!! " + text))
    else:
        L.append(paint.c(GRN, "budget OK"))
    return [ln[:width * 2] for ln in L]


def _dur(s):
    if s is None:
        return "?"
    s = int(s)
    return "%dd%02dh%02dm" % (s // 86400, s // 3600 % 24, s // 60 % 60) if s >= 86400 else \
        "%dh%02dm%02ds" % (s // 3600, s // 60 % 60, s % 60)


def _mhz(hz):
    return "-" if hz is None else "%.4f" % (hz / 1e6)


def _mib(v):
    return "-" if v is None else "%.1fM" % (v / 1048576.0)


def follow_log(board, state, path):
    """Append new log ring text to path; state holds the next offset."""
    try:
        r = board.log(state.get("next", 0))
    except client.BoardError:
        return
    if "next" not in state:
        state["next"] = r["next"]  # start at the current end: the past is in `log` on the board
        return
    with open(path, "a", encoding="utf-8") as f:
        if r.get("gap"):
            f.write("\n[pluto-watch: log gap, ring wrapped]\n")
        f.write(r.get("text", ""))
    state["next"] = r["next"]


def add_common(ap):
    ap.add_argument("--deadline-cpu", type=int, default=1, help="deadline core (1 in layout light)")
    ap.add_argument("--max-deadline", type=float, default=60.0, help="deadline core busy limit %%")
    ap.add_argument("--max-other", type=float, default=60.0, help="other core busy limit %% (>= 40 %% idle)")
    ap.add_argument("--no-wifi-check", action="store_true", help="board on USB Ethernet only")


def budget_from(args):
    return metrics.Budget(deadline_cpu=args.deadline_cpu, deadline_max_pct=args.max_deadline,
                          other_max_pct=args.max_other, require_wifi=not args.no_wifi_check)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="pluto-watch", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board", nargs="?", default=os.environ.get("PLUTO_HOST"), help="board ip[:port] ($PLUTO_HOST)")
    ap.add_argument("-i", "--interval", type=float, default=2.0)
    ap.add_argument("--record", help="append every poll as JSONL {host_t, metrics}")
    ap.add_argument("--log-file", help="follow the board log ring into this file")
    ap.add_argument("--full-every", type=float, default=30.0, help="seconds between full=1 polls (0 = never)")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--no-hw", action="store_true", help="skip the AD9363 readback (driver lock)")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--count", type=int, default=0, help="stop after N polls")
    ap.add_argument("--no-color", action="store_true")
    add_common(ap)
    args = ap.parse_args(argv)
    if not args.board:
        ap.error("board address required (or PLUTO_HOST)")
    board = client.Board(args.board)
    budget = budget_from(args)
    paint = Painter(not args.no_color and sys.stdout.isatty())
    rec = open(args.record, "a", encoding="utf-8") if args.record else None
    prev_flat = start_flat = None
    last_full = 0.0
    log_state = {}
    polls = 0
    worst = 0
    try:
        while True:
            t0 = time.monotonic()
            full = args.full_every > 0 and (t0 - last_full >= args.full_every or args.once)
            try:
                m = board.metrics(top=args.top, full=full, hw=not args.no_hw)
            except client.BoardError as e:
                line = paint.c(RED, "!! %s %s" % (time.strftime("%H:%M:%S"), e))
                print(line if args.once else "\033[2J\033[H" + line, flush=True)
                if rec:
                    rec.write(json.dumps({"host_t": time.time(), "error": str(e)}) + "\n")
                    rec.flush()
                worst = 2
                if args.once:
                    return 2
                time.sleep(args.interval)
                continue
            if full:
                last_full = t0
            flat = metrics.flatten(m)
            if start_flat is None:
                start_flat = flat
            flags = metrics.check_sample(prev_flat, flat, budget)
            if any(f[0] == "fail" for f in flags):
                worst = max(worst, 1)
            lines = render(m, flat, prev_flat, flags, start_flat, paint)
            if args.once:
                print("\n".join(lines))
            else:
                sys.stdout.write("\033[2J\033[H" + "\n".join(lines) + "\n" +
                                 paint.c(DIM, "%s  poll %d  (Ctrl-C to stop)" % (time.strftime("%H:%M:%S"), polls + 1)) + "\n")
                sys.stdout.flush()
            if rec:
                rec.write(json.dumps({"host_t": time.time(), "metrics": m}) + "\n")
                rec.flush()
            if args.log_file:
                follow_log(board, log_state, args.log_file)
            prev_flat = flat
            polls += 1
            if args.once or (args.count and polls >= args.count):
                return worst
            time.sleep(max(0.0, args.interval - (time.monotonic() - t0)))
    except KeyboardInterrupt:
        return worst
    finally:
        if rec:
            rec.close()
