"""pluto-modes: per-mode cost matrix and isolated, idempotent per-mode control check.

  pluto-modes 192.168.1.160 --dash http://192.168.1.160:8080 --window 60 --cycles 3
  pluto-modes 192.168.1.160 --dash-user admin --dash-pass-env NEXUS_DASH_PASS --skip-cost

Through the nexus2 dashboard (GET /api/status, POST /api/control/native; address the board by
IP: without dashboard credentials nexus2 answers only Host = its own ip:port):

1. cost matrix: TETRA only, then +DMR, +P25, +FM (one channel per mode, the others of these
   four disabled); per step after --settle s, --window s of /metrics: CPU0/CPU1 busy, drive.turn
   / rx.analysis / tx.synthesis / tetra.tick window means, each carrier's us/s, tx.late.
2. isolation: all four enabled; each toggled mode goes off and on --cycles times (hold --hold s)
   while every other bound channel is sampled every --sample s: service_turns must keep increasing
   (TETRA's is its slot counter), their worker phase stay bound, their intent revision and
   desired state stay unchanged; tx.late must not move. Every apply repeats the same intent
   once more, which must come back 200 unchanged with the revision untouched (idempotent).
The channels' initial desired states are restored at the end.
"""

import argparse
import json
import os
import sys
import threading
import time

from . import client, common, metrics, stats

MODES = ("tetra", "dmr", "p25", "fm")


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def pick_channels(status, modes=MODES):
    """{mode: channel dict} (first controllable channel of each mode; TETRA packet-data
    carriers excluded)."""
    out = {}
    for ch in status.get("channels") or []:
        mode = ch.get("mode")
        if mode in modes and mode not in out and ch.get("tetra_role") != "packet_data" \
                and ch.get("native_control") is not None:
            out[mode] = ch
    return out


def chan(status, cid):
    for ch in status.get("channels") or []:
        if ch.get("id") == cid:
            return ch
    return None


def applied(ch, want):
    nc = (ch or {}).get("native_control") or {}
    if nc.get("desired_enabled") != want or nc.get("stage") != "idle":
        return False
    phase = nc.get("worker_phase")
    return phase == "bound" if want else phase != "bound"


class ModeControl:
    def __init__(self, dash, apply_timeout=30.0):
        self.dash = dash
        self.apply_timeout = apply_timeout

    def set(self, cid, want, check_idempotent=True):
        """Submit (if needed) and wait until applied. Returns a dict with timings/results."""
        st = self.dash.status()
        ch = chan(st, cid)
        if ch is None:
            raise client.BoardError("channel %s vanished" % cid)
        res = {"channel": cid, "enabled": want}
        t0 = time.monotonic()
        if not applied(ch, want):
            rev = (ch.get("native_control") or {}).get("intent_revision")
            code, receipt = self.dash.native(cid, want, rev)
            res["submit"] = {"http": code, "receipt": receipt}
            if code not in (200, 202):
                raise client.BoardError("native %s enabled=%s: HTTP %d %s" % (cid, want, code, receipt))
            while True:
                st = self.dash.status()
                ch = chan(st, cid)
                if applied(ch, want):
                    break
                if time.monotonic() - t0 > self.apply_timeout:
                    raise client.BoardError("native %s enabled=%s not applied in %.0f s (stage %s, phase %s)" % (
                        cid, want, self.apply_timeout, (ch.get("native_control") or {}).get("stage"),
                        (ch.get("native_control") or {}).get("worker_phase")))
                time.sleep(0.2)
        res["apply_ms"] = (time.monotonic() - t0) * 1000.0
        if check_idempotent:
            nc = ch.get("native_control") or {}
            rev = nc.get("intent_revision")
            code, receipt = self.dash.native(cid, want, rev)
            after = (chan(self.dash.status(), cid) or {}).get("native_control") or {}
            res["repeat"] = {"http": code, "status": receipt.get("status"),
                             "revision_before": rev, "revision_after": after.get("intent_revision")}
            res["idempotent"] = code == 200 and receipt.get("status") == "unchanged" and \
                after.get("intent_revision") == rev and applied(chan(self.dash.status(), cid), want)
        return res


def measure(board, seconds, interval, rec, tag):
    samples, errors = common.sample(board, seconds, interval, rec, tag)
    out = {"polls": len(samples), "poll_errors": errors}
    if len(samples) < 2:
        return out
    body = samples[1:]
    for cpu in ("cpu0", "cpu1"):
        out[cpu] = stats.summary([s.get(cpu) for s in body])["mean"]
    first, last = samples[0], samples[-1]
    for name in ("drive.turn", "rx.analysis", "tx.synthesis", "tetra.tick", "tx.write"):
        out[name] = metrics.stage_window(first, last, name)[0]
    out["carriers"] = {}
    for mode in MODES:
        n = metrics.carrier_stage(last, mode)
        if n:
            out["carriers"][mode] = metrics.stage_window(first, last, n)[1]
    out["tx_late"] = stats.delta(first.get("tx_late"), last.get("tx_late"))
    return out


class Sampler(threading.Thread):
    """Dashboard + /metrics poller during the toggles (own connections per request)."""

    def __init__(self, dash, board, period, rec):
        super().__init__(daemon=True)
        self.dash, self.board, self.period, self.rec = dash, board, period, rec
        self.rows = []  # (t, {cid: (service_turns, phase, revision, desired)}, tx_late)
        self.stop_ev = threading.Event()
        self.errors = 0

    def run(self):
        while not self.stop_ev.is_set():
            t = time.monotonic()
            try:
                st = self.dash.status()
                chans = {}
                for ch in st.get("channels") or []:
                    nc = ch.get("native_control") or {}
                    chans[ch.get("id")] = (ch.get("service_turns"), nc.get("worker_phase"),
                                           nc.get("intent_revision"), nc.get("desired_enabled"))
                late = None
                try:
                    late = metrics.flatten(self.board.metrics(top=4, hw=False)).get("tx_late")
                except client.BoardError:
                    pass
                self.rows.append((t, chans, late))
                if self.rec:
                    self.rec.write(tag="isolation", channels=chans, tx_late=late)
            except (client.BoardError, ValueError):
                self.errors += 1
            self.stop_ev.wait(max(0.0, self.period - (time.monotonic() - t)))

    def window(self, t0, t1):
        return [r for r in self.rows if t0 <= r[0] <= t1]


def continuity(rows, others):
    """Violations of the untouched channels over consecutive samples."""
    v = {"stalls": 0, "phase_changes": 0, "revision_changes": 0, "desired_changes": 0, "samples": len(rows)}
    for (_, a, _), (_, b, _) in zip(rows, rows[1:]):
        for cid in others:
            if cid not in a or cid not in b:
                v["stalls"] += 1
                continue
            ta, pa, ra, da = a[cid]
            tb, pb, rb, db = b[cid]
            if ta is not None and tb is not None and tb <= ta:
                v["stalls"] += 1
            if pb != pa or pb != "bound":
                v["phase_changes"] += 1
            if rb != ra:
                v["revision_changes"] += 1
            if db != da:
                v["desired_changes"] += 1
    lates = [r[2] for r in rows if r[2] is not None]
    v["tx_late_delta"] = (lates[-1] - lates[0]) if len(lates) >= 2 else None
    return v


def main(argv=None):
    ap = argparse.ArgumentParser(prog="pluto-modes", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    common.add_board_args(ap)
    ap.add_argument("--dash", help="dashboard URL (default http://<board>:8080)")
    ap.add_argument("--dash-user")
    ap.add_argument("--dash-pass-env", help="environment variable with the dashboard password")
    ap.add_argument("--modes", default="dmr,p25,fm", help="modes added after TETRA / toggled")
    ap.add_argument("--include-tetra", action="store_true", help="toggle TETRA too")
    ap.add_argument("--settle", type=float, default=15.0)
    ap.add_argument("--window", type=float, default=60.0)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--cycles", type=int, default=3)
    ap.add_argument("--hold", type=float, default=15.0)
    ap.add_argument("--sample", type=float, default=1.0, help="isolation sampling period")
    ap.add_argument("--apply-timeout", type=float, default=30.0)
    ap.add_argument("--skip-cost", action="store_true")
    ap.add_argument("--skip-isolation", action="store_true")
    ap.add_argument("--no-restore", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--record")
    args = ap.parse_args(argv)
    board, _ = common.board_from(args)
    password = os.environ.get(args.dash_pass_env) if args.dash_pass_env else None
    dash = client.Dashboard(args.dash or "http://%s:8080" % board.host, args.dash_user, password)
    ctl = ModeControl(dash, args.apply_timeout)
    rec = common.Recorder(args.record)
    added = [m.strip() for m in args.modes.split(",") if m.strip()]
    report = {"cost": [], "isolation": []}

    st = dash.status()
    picked = pick_channels(st)
    if "tetra" not in picked:
        log("no controllable TETRA channel in /api/status")
        return 2
    missing = [m for m in added if m not in picked]
    if missing:
        log("no controllable channel for %s: left out" % ", ".join(missing))
    added = [m for m in added if m in picked]
    ids = {m: picked[m]["id"] for m in ["tetra"] + added}
    initial = {cid: ((chan(st, cid) or {}).get("native_control") or {}).get("desired_enabled") for cid in ids.values()}
    log("channels: " + ", ".join("%s=%s" % (m, c) for m, c in ids.items()))
    rc = 0
    try:
        if not args.skip_cost:
            steps = [["tetra"] + added[:i] for i in range(len(added) + 1)]
            prev_cpu1 = None
            for on in steps:
                for m, cid in ids.items():
                    ctl.set(cid, m in on, check_idempotent=False)
                log("cost step %s: settle %.0f s, window %.0f s" % ("+".join(on), args.settle, args.window))
                time.sleep(args.settle)
                r = measure(board, args.window, args.interval, rec, "cost:" + "+".join(on))
                r["modes"] = list(on)
                r["cpu1_delta"] = stats.delta(prev_cpu1, r.get("cpu1"))
                prev_cpu1 = r.get("cpu1")
                report["cost"].append(r)
            print()
            hdr = "%-20s %6s %6s %7s %9s %9s %9s %9s %8s" % ("modes", "cpu0", "cpu1", "dcpu1", "drive_us",
                                                             "analys_us", "synth_us", "tetra_us", "tx_late")
            print(hdr + "".join(" %8s" % ("%s us/s" % m)[:8] for m in MODES))
            for r in report["cost"]:
                line = "%-20s %6s %6s %7s %9s %9s %9s %9s %8s" % (
                    "+".join(r["modes"]), stats.fmt(r.get("cpu0")), stats.fmt(r.get("cpu1")),
                    stats.fmt(r.get("cpu1_delta")), stats.fmt(r.get("drive.turn")), stats.fmt(r.get("rx.analysis")),
                    stats.fmt(r.get("tx.synthesis")), stats.fmt(r.get("tetra.tick")), stats.fmt(r.get("tx_late")))
                print(line + "".join(" %8s" % stats.fmt((r.get("carriers") or {}).get(m), 0) for m in MODES))
            print()

        if not args.skip_isolation:
            for cid in ids.values():
                ctl.set(cid, True, check_idempotent=False)
            time.sleep(args.settle)
            sampler = Sampler(dash, board, args.sample, rec)
            sampler.start()
            try:
                toggles = (["tetra"] if args.include_tetra else []) + added
                for m in toggles:
                    cid = ids[m]
                    # every other channel bound now (not only the four picked: a packet-data
                    # carrier or a second channel of a mode must not be disturbed either)
                    others = [ch.get("id") for ch in dash.status().get("channels") or []
                              if ch.get("id") != cid and
                              ((ch.get("native_control") or {}).get("worker_phase") == "bound")]
                    t0 = time.monotonic()
                    offs, ons, idem = [], [], []
                    for cyc in range(args.cycles):
                        r = ctl.set(cid, False)
                        offs.append(r["apply_ms"])
                        idem.append(r.get("idempotent"))
                        time.sleep(args.hold)
                        r = ctl.set(cid, True)
                        ons.append(r["apply_ms"])
                        idem.append(r.get("idempotent"))
                        time.sleep(args.hold)
                        log("%s cycle %d: off %.0f ms, on %.0f ms" % (m, cyc + 1, offs[-1], ons[-1]))
                    v = continuity(sampler.window(t0, time.monotonic()), others)
                    v.update({"mode": m, "channel": cid, "cycles": args.cycles,
                              "off_ms": stats.summary(offs), "on_ms": stats.summary(ons),
                              "idempotent": all(x is True for x in idem)})
                    ok = v["idempotent"] and v["stalls"] == 0 and v["phase_changes"] == 0 and \
                        v["revision_changes"] == 0 and v["desired_changes"] == 0 and not v["tx_late_delta"]
                    v["verdict"] = "PASS" if ok else "FAIL"
                    report["isolation"].append(v)
            finally:
                sampler.stop_ev.set()
                sampler.join(5)
            print("%-6s %-10s %6s %15s %15s %6s %6s %6s %6s %7s %7s %s" % (
                "mode", "channel", "cycles", "off ms mean/max", "on ms mean/max", "idem", "stall", "phase",
                "rev", "desired", "txlate", "verdict"))
            for v in report["isolation"]:
                print("%-6s %-10s %6d %15s %15s %6s %6d %6d %6d %7d %7s %s" % (
                    v["mode"], v["channel"][:10], v["cycles"],
                    "%s/%s" % (stats.fmt(v["off_ms"]["mean"], 0), stats.fmt(v["off_ms"]["max"], 0)),
                    "%s/%s" % (stats.fmt(v["on_ms"]["mean"], 0), stats.fmt(v["on_ms"]["max"], 0)),
                    "yes" if v["idempotent"] else "NO", v["stalls"], v["phase_changes"], v["revision_changes"],
                    v["desired_changes"], stats.fmt(v["tx_late_delta"]), v["verdict"]))
                if v["verdict"] != "PASS":
                    rc = 1
    except client.BoardError as e:
        log("FAILED: %s" % e)
        rc = 2
    except KeyboardInterrupt:
        log("interrupted")
        rc = 2
    finally:
        if not args.no_restore:
            for cid, want in initial.items():
                if want is None:
                    continue
                try:
                    ctl.set(cid, want, check_idempotent=False)
                except client.BoardError as e:
                    log("restore %s: %s" % (cid, e))
            log("initial channel states restored")
        rec.close()
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
        log("written %s" % args.out)
    return rc


if __name__ == "__main__":
    sys.exit(main())
