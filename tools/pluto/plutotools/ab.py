"""pluto-ab: scenario A then B (optionally repeated), /metrics collected, compared, judged.

  # two switch sets (nexus2 restarted for each; needs PLUTO_TOKEN or --ssh-key)
  pluto-ab 192.168.1.160 --minutes 10 --warmup 60 \\
      --a-label q15-off --a-env NEXUS2_PLUTO_ANALYSIS_Q15=0 \\
      --b-label q15-on  --b-env NEXUS2_PLUTO_ANALYSIS_Q15=1 --rounds 2 --out ab.json
  # two layouts
  pluto-ab 192.168.1.160 --a-layout light --b-layout cpu1 --minutes 5
  # two builds (flashes the board twice per round: asks first unless --yes-flash)
  pluto-ab 192.168.1.160 --a-image rtos-ci-410 --b-image rtos-ci-412 --ssh-key k --known-hosts kh
  # anything else: set it up by hand when asked
  pluto-ab 192.168.1.160 --manual --a-label before --b-label after

Each run: apply the scenario, warm up, `nexus stages reset` (when a command path exists),
poll /metrics every --interval for --minutes, then summarise: CPU0/CPU1 busy mean/p99/max over
the polls, per stage mean/p99/max/over (cumulative since the reset; window means without
one), counter deltas (tx.late, USB errors/handshake misses/resets, ...), Wi-Fi drops, heap
minimum. Verdict per run against the budget (deadline core mean <= --max-deadline, other core
mean <= --max-other, tx.late 0, no USB error counter moving, Wi-Fi kept, no reboot). Env
switches stay set for later starts: give B every switch A changed (NAME= unsets one).
"""

import argparse
import json
import sys
import time

from . import client, common, deploy, metrics, stats
from .watch import add_common, budget_from

USB_KEYS = tuple(k for k in metrics.FAIL_COUNTERS if k.startswith("usb."))


class Scenario:
    def __init__(self, label, env=None, layout=None, image=None, cmds=None, manual=False):
        self.label = label
        self.env = env or []
        self.layout = layout
        self.image = image
        self.cmds = cmds or []
        self.manual = manual

    @property
    def restarts(self):
        return bool(self.env or self.layout)

    def describe(self):
        parts = []
        if self.image:
            parts.append("image " + self.image)
        if self.layout:
            parts.append("layout " + self.layout)
        if self.env:
            parts.append("env " + " ".join(self.env))
        parts += ["cmd '%s'" % c for c in self.cmds]
        if self.manual:
            parts.append("manual")
        return ", ".join(parts) or "as is"


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def apply(scn, board, runner, args):
    if scn.image:
        argv = [board.host, scn.image, "--asset", args.asset]
        if args.ssh_key:
            argv += ["--ssh-key", args.ssh_key]
        if args.known_hosts:
            argv += ["--known-hosts", args.known_hosts]
        if args.yes_flash:
            argv.append("--yes")
        rc = deploy.main(argv)
        if rc not in (0,):
            raise client.BoardError("deploy of %s failed (%d)" % (scn.image, rc))
        common.wait_state(board, "running", 180)
    if scn.restarts:
        if not runner.available:
            raise client.BoardError("env/layout scenarios need a command path (PLUTO_TOKEN or --ssh-key)")
        log("nexus stop")
        runner.run("nexus stop")
        common.wait_state(board, "stopped", 60)
        if scn.layout:
            runner.run("nexus layout " + scn.layout)
        for i in range(0, len(scn.env), 8):
            runner.run("nexus env " + " ".join(scn.env[i:i + 8]))
        log("nexus start")
        runner.run("nexus start")
        common.wait_state(board, "running", 90)
    for c in scn.cmds:
        out = runner.run(c)
        log("%s: %s" % (c, out.strip().splitlines()[-1] if out.strip() else "ok"))
    if scn.manual:
        input("Set up scenario %s (%s), then press Enter " % (scn.label, scn.describe()))


def analyze(samples, errors, stages_reset):
    """Summary of one run's flattened samples."""
    a = {"polls": len(samples), "poll_errors": errors}
    if not samples:
        return a
    body = samples[1:] if len(samples) > 2 else samples  # first interval spans the setup
    for cpu in ("cpu0", "cpu1"):
        a[cpu] = stats.summary([s.get(cpu) for s in body])
    first, last = samples[0], samples[-1]
    a["duration_s"] = (last.get("uptime_s") or 0) - (first.get("uptime_s") or 0)
    st = {}
    for name in metrics.stage_names(last):
        wmean, usps = metrics.stage_window(first, last, name)
        row = {"window_mean_us": wmean, "us_per_s": usps}
        if stages_reset:
            for k in ("mean_us", "p99_us", "max_us", "over", "count"):
                row[k] = last.get("stage.%s.%s" % (name, k))
        else:
            row["over"] = stats.delta(first.get("stage.%s.over" % name), last.get("stage.%s.over" % name))
            row["max_us"] = last.get("stage.%s.max_us" % name)  # cumulative since start
        st[name] = row
    a["stages"] = st
    a["counters"] = metrics.counter_deltas(first, last, metrics.FAIL_COUNTERS + metrics.WARN_COUNTERS)
    a["wifi_down_polls"] = sum(1 for s in samples if s.get("wifi.connected") is False)
    a["reboot"] = any((b.get("uptime_s") or 0) < (p.get("uptime_s") or 0) for p, b in zip(samples, samples[1:]))
    a["nexus2_not_running_polls"] = sum(1 for s in samples if s.get("nexus2.state") not in ("running", None))
    mins = [s.get("heap.min_free") for s in samples if s.get("heap.min_free") is not None]
    a["heap_min_free"] = min(mins) if mins else None
    temps = [s.get("temp.zynq_c") for s in samples]
    a["zynq_c"] = stats.summary(temps)["mean"]
    a["build"] = last.get("build.git")
    return a


def verdict(a, budget):
    """(PASS|FAIL, [reasons])"""
    r = []
    d = (a.get("cpu%d" % budget.deadline_cpu) or {}).get("mean")
    o = (a.get("cpu%d" % budget.other_cpu) or {}).get("mean")
    if d is None:
        r.append("no CPU data")
    elif d > budget.deadline_max_pct:
        r.append("deadline CPU%d mean %.1f %% > %.0f %%" % (budget.deadline_cpu, d, budget.deadline_max_pct))
    if o is not None and o > budget.other_max_pct:
        r.append("CPU%d mean %.1f %% busy (< %.0f %% idle)" % (budget.other_cpu, o, 100 - budget.other_max_pct))
    c = a.get("counters") or {}
    if (c.get("tx_late") or 0) > 0:
        r.append("tx.late +%d" % c["tx_late"])
    for k in USB_KEYS:
        if (c.get(k) or 0) > 0:
            r.append("%s +%d" % (k, c[k]))
    if budget.require_wifi and a.get("wifi_down_polls"):
        r.append("Wi-Fi down in %d polls" % a["wifi_down_polls"])
    if a.get("reboot"):
        r.append("board rebooted")
    if a.get("nexus2_not_running_polls"):
        r.append("nexus2 not running in %d polls" % a["nexus2_not_running_polls"])
    if a.get("polls", 0) == 0 or a.get("poll_errors", 0) > 0.1 * max(1, a.get("polls", 0)):
        r.append("board unreachable in %d polls" % a.get("poll_errors", 0))
    return ("FAIL" if r else "PASS"), r


def combine(runs):
    """Mean of the per-run values (for repeated rounds)."""
    def mean(vals):
        v = [x for x in vals if x is not None]
        return sum(v) / len(v) if v else None
    out = {}
    for cpu in ("cpu0", "cpu1"):
        out[cpu] = {k: mean([(r.get(cpu) or {}).get(k) for r in runs]) for k in ("mean", "p99", "max")}
    names = sorted({n for r in runs for n in (r.get("stages") or {})})
    out["stages"] = {}
    for n in names:
        rows = [(r.get("stages") or {}).get(n) or {} for r in runs]
        out["stages"][n] = {k: mean([row.get(k) for row in rows])
                            for k in ("mean_us", "window_mean_us", "p99_us", "max_us", "over", "us_per_s")}
    keys = sorted({k for r in runs for k in (r.get("counters") or {})})
    out["counters"] = {k: sum((r.get("counters") or {}).get(k) or 0 for r in runs) for k in keys}
    out["wifi_down_polls"] = sum(r.get("wifi_down_polls", 0) for r in runs)
    hm = [r.get("heap_min_free") for r in runs if r.get("heap_min_free") is not None]
    out["heap_min_free"] = min(hm) if hm else None
    out["zynq_c"] = mean([r.get("zynq_c") for r in runs])
    return out


def table(la, lb, A, B):
    """Comparison rows: (metric, a, b, delta, delta %)."""
    rows = []

    def add(name, a, b, digits=1):
        rows.append((name, stats.fmt(a, digits), stats.fmt(b, digits), stats.fmt(stats.delta(a, b), digits),
                     stats.fmt(stats.pct_change(a, b), 1)))
    for cpu in ("cpu0", "cpu1"):
        for k in ("mean", "p99", "max"):
            add("%s busy %s %%" % (cpu, k), A[cpu][k], B[cpu][k])
    for n in sorted(set(A["stages"]) | set(B["stages"])):
        sa, sb = A["stages"].get(n, {}), B["stages"].get(n, {})
        mean_key = "mean_us" if sa.get("mean_us") is not None else "window_mean_us"
        add("%s mean us" % n, sa.get(mean_key), sb.get(mean_key))
        if sa.get("p99_us") is not None or sb.get("p99_us") is not None:
            add("%s p99 us" % n, sa.get("p99_us"), sb.get("p99_us"), 0)
        add("%s max us" % n, sa.get("max_us"), sb.get("max_us"), 0)
        add("%s over" % n, sa.get("over"), sb.get("over"), 0)
        add("%s us/s" % n, sa.get("us_per_s"), sb.get("us_per_s"), 0)
    for k in sorted(set(A["counters"]) | set(B["counters"])):
        if A["counters"].get(k) or B["counters"].get(k):
            add("%s (delta)" % k, A["counters"].get(k), B["counters"].get(k), 0)
    add("wifi down polls", A["wifi_down_polls"], B["wifi_down_polls"], 0)
    add("heap min free MiB", (A["heap_min_free"] or 0) / 1048576.0 if A["heap_min_free"] is not None else None,
        (B["heap_min_free"] or 0) / 1048576.0 if B["heap_min_free"] is not None else None, 2)
    add("zynq temp C", A["zynq_c"], B["zynq_c"])
    return rows


def print_table(la, lb, rows):
    w = max([len(r[0]) for r in rows] + [10])
    print("%-*s %12s %12s %12s %8s" % (w, "metric", la[:12], lb[:12], "delta", "delta%"))
    for r in rows:
        print("%-*s %12s %12s %12s %8s" % (w, r[0], r[1], r[2], r[3], r[4]))


def main(argv=None):
    ap = argparse.ArgumentParser(prog="pluto-ab", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    common.add_board_args(ap)
    for x in ("a", "b"):
        ap.add_argument("--%s-label" % x, default=x.upper())
        ap.add_argument("--%s-env" % x, action="append", default=[], help="NEXUS2_NAME=V (repeatable)")
        ap.add_argument("--%s-layout" % x, choices=("light", "cpu1", "float"))
        ap.add_argument("--%s-image" % x, help="local .itb or CI release tag (flashes the board)")
        ap.add_argument("--%s-cmd" % x, action="append", default=[], help="allowlisted /cmd to run")
    ap.add_argument("--manual", action="store_true", help="ask the operator to set up each scenario")
    ap.add_argument("--asset", default=deploy.DEFAULT_ASSET)
    ap.add_argument("--yes-flash", action="store_true", help="flash image scenarios without asking")
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--warmup", type=float, default=60.0, help="seconds after applying, before measuring")
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--rounds", type=int, default=1, help="A,B repeated (repeatability: >= 2)")
    ap.add_argument("--out", help="write all runs, summaries and verdicts as JSON")
    ap.add_argument("--record", help="every poll as JSONL")
    add_common(ap)
    args = ap.parse_args(argv)
    board, runner = common.board_from(args)
    budget = budget_from(args)

    def env_words(lst):
        out = []
        for item in lst:
            out += item.split()
        for kv in out:
            if "=" not in kv or not (kv.startswith("NEXUS2_") or kv.startswith("RUST_LOG=")):
                raise SystemExit("bad env switch %r (NEXUS2_NAME=VALUE)" % kv)
        return out

    A = Scenario(args.a_label, env_words(args.a_env), args.a_layout, args.a_image, args.a_cmd, args.manual)
    B = Scenario(args.b_label, env_words(args.b_env), args.b_layout, args.b_image, args.b_cmd, args.manual)
    rec = common.Recorder(args.record)
    runs = {A.label: [], B.label: []}
    report = {"budget": vars(budget), "scenarios": {A.label: A.describe(), B.label: B.describe()}, "runs": []}
    try:
        for rnd in range(args.rounds):
            for scn in (A, B):
                log("round %d/%d scenario %s: %s" % (rnd + 1, args.rounds, scn.label, scn.describe()))
                apply(scn, board, runner, args)
                if args.warmup > 0:
                    log("warm-up %.0f s" % args.warmup)
                    time.sleep(args.warmup)
                reset = False
                if runner.available:
                    try:
                        runner.run("nexus stages reset")
                        reset = True
                    except client.BoardError as e:
                        log("stages not reset (%s): window means only" % e)
                log("measuring %.1f min" % args.minutes)
                samples, errors = common.sample(board, args.minutes * 60, args.interval, rec,
                                                tag="%s#%d" % (scn.label, rnd + 1))
                a = analyze(samples, errors, reset)
                v, why = verdict(a, budget)
                a["verdict"], a["reasons"] = v, why
                runs[scn.label].append(a)
                report["runs"].append({"label": scn.label, "round": rnd + 1, "summary": a})
                log("%s round %d: %s%s  cpu0 %s cpu1 %s" % (
                    scn.label, rnd + 1, v, (" (" + "; ".join(why) + ")") if why else "",
                    stats.fmt((a.get("cpu0") or {}).get("mean")), stats.fmt((a.get("cpu1") or {}).get("mean"))))
    except KeyboardInterrupt:
        log("interrupted: comparing what was measured")
    except client.BoardError as e:
        log("FAILED: %s" % e)
        if not runs[A.label] or not runs[B.label]:
            return 2
    finally:
        rec.close()
    if not runs[A.label] or not runs[B.label]:
        log("nothing to compare")
        return 2
    CA, CB = combine(runs[A.label]), combine(runs[B.label])
    print()
    print_table(A.label, B.label, table(A.label, B.label, CA, CB))
    print()
    if args.rounds > 1:
        for label in (A.label, B.label):
            print("repeatability %s: cpu1 mean per round %s" % (
                label, ", ".join(stats.fmt((r.get("cpu1") or {}).get("mean")) for r in runs[label])))
    result = 0
    for label in (A.label, B.label):
        verdicts = [r["verdict"] for r in runs[label]]
        overall = "PASS" if all(v == "PASS" for v in verdicts) else "FAIL"
        reasons = sorted({x for r in runs[label] for x in r["reasons"]})
        print("%s: %s%s" % (label, overall, ("  " + "; ".join(reasons)) if reasons else ""))
        report.setdefault("verdict", {})[label] = {"verdict": overall, "reasons": reasons}
        if overall != "PASS":
            result = 1
    report["combined"] = {A.label: CA, B.label: CB}
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
        log("written %s" % args.out)
    return result


if __name__ == "__main__":
    sys.exit(main())
