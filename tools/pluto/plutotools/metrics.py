"""Flattening of nexus.metrics/1 documents and the budget rules (rtos-optimizer measure.md:
deadline core <= ~50-60 %, other core >= 40 % idle, tx.late 0, no USB errors, link kept)."""

from dataclasses import dataclass, field

# counters that must not move during a run (any increase = FAIL)
FAIL_COUNTERS = (
    "tx_late",
    "usb.errors",
    "usb.handshake_misses",
    "usb.resets",
    "usb.reset_failures",
    "usb.board_resets",
    "usb.hse",
    "usb.wedges",
    "usb.stuck_cancels",
    "usb.stuck_pending",
    "usb.dma_halts",
    "usb.kill_busy",
    "usb.fatal",
)
# counters reported as warnings when they move
WARN_COUNTERS = (
    "ad9363.tx_discarded",
    "usb.stalls",
    "usb.xacterr_restarts",
    "usb.halted",
    "heap.failed_allocs",
)


def flatten(m):
    """One flat dict of the numbers the tools compare; missing sections give no keys."""
    f = {}
    f["uptime_s"] = m.get("uptime_s")
    b = m.get("build") or {}
    f["build.git"] = b.get("git")
    f["build.image"] = b.get("image")
    cpu = m.get("cpu") or {}
    busy = cpu.get("busy_pct") or []
    for i, v in enumerate(busy):
        f["cpu%d" % i] = v
    f["cpu.interval_s"] = cpu.get("interval_s")
    n2 = m.get("nexus2") or {}
    f["nexus2.state"] = n2.get("state")
    f["nexus2.layout"] = n2.get("layout")
    st = m.get("stages") or {}
    for it in st.get("items") or []:
        name = it.get("name")
        for k in ("count", "mean_us", "p99_us", "max_us", "over", "us_per_s", "budget_us"):
            f["stage.%s.%s" % (name, k)] = it.get(k)
    if st:
        f["tx_late"] = st.get("tx_late")
    usb = m.get("usb") or {}
    for k, v in usb.items():
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            f["usb." + k] = v
    ad = m.get("ad9363") or {}
    for k, v in ad.items():
        f["ad9363." + k] = v
    net = m.get("net") or {}
    wifi = net.get("wifi")
    if wifi is not None:
        f["wifi.connected"] = bool(wifi.get("connected"))
        f["wifi.rssi_dbm"] = wifi.get("rssi_dbm")
        f["wifi.attempts"] = wifi.get("connect_attempts")
    dr = net.get("default_route")
    f["net.default_route"] = ("%s via %s" % (dr.get("gw"), dr.get("if"))) if dr else None
    for i in net.get("ifaces") or []:
        for k in ("ierrors", "oerrors", "iqdrops"):
            if k in i:
                f["net.%s.%s" % (i.get("name"), k)] = i[k]
        f["net.%s.link" % i.get("name")] = i.get("link")
    heap = m.get("heap") or {}
    for k in ("free", "min_free", "largest_free", "failed_allocs"):
        if k in heap:
            f["heap." + k] = heap[k]
    t = m.get("temp") or {}
    f["temp.zynq_c"] = t.get("zynq_c")
    f["collect_ms"] = m.get("collect_ms")
    return f


def stage_names(flat):
    names = []
    for k in flat:
        if k.startswith("stage.") and k.endswith(".mean_us"):
            names.append(k[len("stage."):-len(".mean_us")])
    return names


def stage_window(prev, cur, name):
    """Mean us and us/s of one stage between two flattened samples (cumulative count/mean)."""
    c0, m0 = prev.get("stage.%s.count" % name), prev.get("stage.%s.mean_us" % name)
    c1, m1 = cur.get("stage.%s.count" % name), cur.get("stage.%s.mean_us" % name)
    t0, t1 = prev.get("uptime_s"), cur.get("uptime_s")
    if None in (c0, m0, c1, m1, t0, t1):
        return None, None
    if c1 < c0:  # reset in between: the window is what came after it
        c0, m0 = 0, 0.0
    dc = c1 - c0
    if dc <= 0 or t1 <= t0:
        return None, 0.0
    dsum = c1 * m1 - c0 * m0
    return dsum / dc, dsum / (t1 - t0)


def carrier_stage(flat, mode):
    """Stage name of the carrier serving `mode` (rf-front stage_timing tags: 4 chars)."""
    tag = mode[:4]
    for name in stage_names(flat):
        if name.startswith("carrier") and name.split(".", 1)[-1] == tag:
            return name
    return None


@dataclass
class Budget:
    deadline_cpu: int = 1
    deadline_max_pct: float = 60.0
    other_max_pct: float = 60.0  # other core >= 40 % idle
    require_wifi: bool = True
    fail_counters: tuple = FAIL_COUNTERS
    warn_counters: tuple = WARN_COUNTERS
    extra: dict = field(default_factory=dict)

    @property
    def other_cpu(self):
        return 0 if self.deadline_cpu == 1 else 1


def check_sample(prev, cur, budget):
    """Flags for one poll: [(severity 'fail'|'warn', key, text)]. prev may be None."""
    flags = []
    d = cur.get("cpu%d" % budget.deadline_cpu)
    o = cur.get("cpu%d" % budget.other_cpu)
    if d is not None and d > budget.deadline_max_pct:
        flags.append(("fail", "cpu%d" % budget.deadline_cpu,
                      "CPU%d %.1f %% > %.0f %%" % (budget.deadline_cpu, d, budget.deadline_max_pct)))
    if o is not None and o > budget.other_max_pct:
        flags.append(("fail", "cpu%d" % budget.other_cpu,
                      "CPU%d %.1f %% busy (< %.0f %% idle)" % (budget.other_cpu, o, 100 - budget.other_max_pct)))
    if budget.require_wifi and "wifi.connected" in cur and not cur["wifi.connected"]:
        flags.append(("fail", "wifi.connected", "Wi-Fi not connected"))
    if prev is not None:
        for sev, keys in (("fail", budget.fail_counters), ("warn", budget.warn_counters)):
            for k in keys:
                a, b = prev.get(k), cur.get(k)
                if isinstance(a, (int, float)) and isinstance(b, (int, float)) and b > a:
                    flags.append((sev, k, "%s +%s" % (k, _num(b - a))))
        for name in stage_names(cur):
            k = "stage.%s.over" % name
            a, b = prev.get(k), cur.get(k)
            if isinstance(a, (int, float)) and isinstance(b, (int, float)) and b > a:
                flags.append(("warn", k, "%s over budget +%s" % (name, _num(b - a))))
        if prev.get("uptime_s") is not None and cur.get("uptime_s") is not None and \
                cur["uptime_s"] < prev["uptime_s"]:
            flags.append(("fail", "uptime_s", "board rebooted"))
        if prev.get("nexus2.state") == "running" and cur.get("nexus2.state") not in ("running", None):
            flags.append(("fail", "nexus2.state", "nexus2 %s" % cur.get("nexus2.state")))
    return flags


def counter_deltas(first, last, keys):
    out = {}
    for k in keys:
        a, b = first.get(k), last.get(k)
        if isinstance(a, (int, float)) and isinstance(b, (int, float)):
            out[k] = b - a
    return out


def _num(v):
    return str(int(v)) if float(v).is_integer() else "%.2f" % v
