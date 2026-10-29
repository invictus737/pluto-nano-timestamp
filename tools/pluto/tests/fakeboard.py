"""A fake Pluto for the host tests: the port 80 status server (/metrics, /log, /cmd) and the
nexus2 dashboard (/api/status, /api/control/native), both on loopback ports, with knobs for
budget violations and mode-control faults. Documents follow nexus.metrics/1 as metrics.c
writes it (tests/test_metrics.py also checks a captured document's shape)."""

import json
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = "0123456789abcdef-test-token"


class Board:
    """Simulated station state."""

    def __init__(self):
        self.lock = threading.Lock()
        self.boot = time.monotonic()
        self.state = "running"
        self.layout = "light"
        self.env = {}
        self.stage_reset_at = time.monotonic()
        self.tx_late = 0
        self.usb_errors = 0
        self.handshake = 0
        self.wifi = True
        self.log = bytearray(b"boot line 1\nboot line 2\n")
        self.log_base = 0  # offset of log[0]
        self.cmds = []
        self.flashed = []
        self.git = "1448c76aaaaa"
        self.late_per_poll = 0  # knob: tx.late growth per /metrics
        self.prev_t = None
        self.channels = {}  # dashboard
        self.fault = None  # "dmr_resets_tetra" | "revision_bump" | None

    # -- metrics --
    def cpu1(self):
        base = 45.0
        if self.env.get("NEXUS2_SLOW") == "1":
            base = 72.0
        if self.layout == "cpu1":
            base += 5
        enabled = [c for c in self.channels.values() if c["desired"] and c["phase"] == "bound"]
        return base + 4.0 * max(0, len(enabled) - 1)

    def metrics(self, full):
        with self.lock:
            now = time.monotonic()
            up = now - self.boot
            interval = (now - self.prev_t) if self.prev_t else up
            self.prev_t = now
            self.tx_late += self.late_per_poll
            run = self.state == "running"
            since = now - self.stage_reset_at
            n = int(since * 400)
            stages = None
            if run:
                items = [
                    {"name": "drive.turn", "count": n, "mean_us": 410.0, "p99_us": 512, "max_us": 1900,
                     "budget_us": 2500, "over": 0, "us_per_s": 164000},
                    {"name": "rx.analysis", "count": n, "mean_us": 300.0 if self.env.get("NEXUS2_SLOW") != "1" else 520.0,
                     "p99_us": 512, "max_us": 800, "budget_us": 2500, "over": 0, "us_per_s": 120000},
                    {"name": "carrier0.tetr", "count": n, "mean_us": 50.0, "p99_us": 64, "max_us": 120,
                     "budget_us": 0, "over": 0, "us_per_s": 20000},
                ]
                for i, (cid, c) in enumerate(sorted(self.channels.items())):
                    if c["mode"] != "tetra" and c["desired"]:
                        items.append({"name": "carrier%d.%s" % (i + 1, c["mode"][:4]), "count": n,
                                      "mean_us": 20.0, "p99_us": 32, "max_us": 60, "budget_us": 0, "over": 0,
                                      "us_per_s": 8000})
                stages = {"items": items, "tx_late": self.tx_late}
            m = {
                "schema": "nexus.metrics/1",
                "uptime_s": round(up, 3),
                "unix_s": time.time(),
                "build": {"git": self.git, "ci_run": "412", "nexus_ref": "15eddf361b15", "built": "Oct 29 2026 21:00:00",
                          "rtems": "6.2.0", "usb_stack": "cherry", "image": "station"},
                "cpu": {"n": 2, "interval_s": round(interval, 3), "busy_pct": [22.5, self.cpu1() if run else 3.0],
                        "busy_pct_boot": [20.0, 40.0], "threads": 97},
                "threads": [
                    {"name": "nexus2-drive", "id": "0b010042", "cpu": 1, "aff": "1", "prio": 234, "pct": 25.1},
                    {"name": "usbh_irq", "id": "0b010007", "cpu": 0, "aff": "0", "prio": 2, "pct": 6.3},
                ],
                "threads_rest_pct": 3.2,
                "nexus2": {"linked": True, "state": self.state, "layout": self.layout},
                "stages": stages,
                "ad9363": {"open": run, "rate_hz": 200000.0, "rx_lo_hz": 431362500, "tx_lo_hz": 438362500,
                           "rx_agc": "manual", "rx_gain_db": 40, "ensm": "fdd", "tx_atten_db": 10.0,
                           "tx_overflow_bits": 0, "tx_discarded": 0, "tx_fir_scale": 1, "temp_c": 41.5},
                "usb": {"stack": "cherry", "hcd": "ehci-nxp", "pipes_open": 6, "submits": int(up * 100),
                        "completions": int(up * 100), "errors": self.usb_errors,
                        "handshake_misses": self.handshake, "resets": 0, "board_resets": 0, "kills": 3,
                        "stalls": 0, "xacterr_restarts": 0, "deferred": 0, "hse": 0, "gate": False},
                "net": {"ifaces": [{"name": "wlan0", "up": True, "running": True, "link": "up" if self.wifi else "down",
                                    "mtu": 1500, "ipackets": 10, "ierrors": 0, "opackets": 9, "oerrors": 0,
                                    "iqdrops": 0, "ipv4": ["192.168.1.160"]}],
                        "default_route": {"gw": "192.168.1.1", "if": "wlan0"} if self.wifi else None,
                        "dns": ["192.168.1.1", "1.1.1.1"],
                        "wifi": {"present": True, "station": True, "connected": self.wifi, "want": True,
                                 "ssid": "Chris", "rssi_dbm": -58 if self.wifi else None,
                                 "channel": 6 if self.wifi else None, "connect_attempts": 1, "last_error": 0}},
                "heap": {"size": 400 << 20, "free": 300 << 20, "min_free": 290 << 20, "free_blocks": 12,
                         "used_blocks": 3000, "failed_allocs": 0, "largest_free": 280 << 20},
                "temp": {"zynq_c": 52.3},
                "log": {"next": self.log_base + len(self.log), "oldest": self.log_base},
                "collect_ms": 1.2,
            }
            if full:
                m["stacks"] = [{"name": "nexus2-drive", "size": 524288, "used": 61234, "pct": 11.7}]
                m["net"]["events"] = ["-    12 s  wlan0  link up"]
            return m

    def log_read(self, since, cap):
        with self.lock:
            end = self.log_base + len(self.log)
            start = min(max(since, self.log_base), end)
            data = bytes(self.log[start - self.log_base:start - self.log_base + cap])
            return {"since": since, "start": start, "next": start + len(data), "oldest": self.log_base,
                    "gap": start != since, "text": data.decode("utf-8", "replace")}

    def append_log(self, text):
        with self.lock:
            self.log += text.encode()

    # -- /cmd --
    def cmd(self, line):
        argv = line.split()
        with self.lock:
            self.cmds.append(line)
        if argv[:2] == ["nexus", "stop"]:
            with self.lock:
                self.state = "stopped"
            return 0, "nexus2: stop requested\n"
        if argv[:2] == ["nexus", "start"]:
            with self.lock:
                self.state = "running"
                self.stage_reset_at = time.monotonic()
            return 0, "nexus2: starting\n"
        if argv[:2] == ["nexus", "env"]:
            for kv in argv[2:]:
                k, _, v = kv.partition("=")
                if v:
                    self.env[k] = v
                else:
                    self.env.pop(k, None)
            return 0, "".join("  %s=%s\n" % kv for kv in sorted(self.env.items()))
        if argv[:2] == ["nexus", "layout"] and len(argv) == 3:
            self.layout = argv[2]
            return 0, "nexus2: layout %s\n" % argv[2]
        if argv[:3] == ["nexus", "stages", "reset"]:
            with self.lock:
                self.stage_reset_at = time.monotonic()
            return 0, "(reset)\n"
        if argv[:2] == ["nexus", "status"]:
            return 0, "nexus2: %s, layout %s\n" % (self.state, self.layout)
        return -1, ""

    # -- dashboard --
    def setup_channels(self):
        for i, mode in enumerate(("tetra", "dmr", "p25", "fm")):
            self.channels["ch-%s" % mode] = {"mode": mode, "desired": True, "phase": "bound", "rev": 10 + i,
                                            "stage": "idle", "turns": 0, "apply_at": None}

    def tick_channels(self):
        now = time.monotonic()
        for cid, c in self.channels.items():
            if c["apply_at"] is not None and now >= c["apply_at"]:
                c["phase"] = "bound" if c["desired"] else "retired"
                c["stage"] = "idle"
                c["apply_at"] = None
            if c["phase"] == "bound":
                c["turns"] += 1

    def dash_status(self):
        with self.lock:
            self.tick_channels()
            chans = []
            for cid, c in sorted(self.channels.items()):
                chans.append({
                    "id": cid, "mode": c["mode"], "enabled": c["desired"], "service_turns": c["turns"],
                    "collected_samples": c["turns"] * 100,
                    "native_control": {"desired_enabled": c["desired"], "intent_revision": str(c["rev"]),
                                       "stage": c["stage"], "worker_phase": c["phase"],
                                       "admission_revision": str(c["rev"]), "available": True, "failed": False},
                })
            return {"assets": 1, "channels": chans, "native_dropped": 0, "runtime": None, "last_heard": []}

    def dash_native(self, req):
        with self.lock:
            self.tick_channels()
            c = self.channels.get(req.get("channel"))
            if c is None:
                return 422, {"id": 1, "status": "rejected"}
            if str(c["rev"]) != req.get("expected_revision"):
                return 409, {"id": 2, "status": "conflict"}
            if c["desired"] == req.get("enabled"):
                return 200, {"id": 3, "status": "unchanged"}
            c["desired"] = bool(req.get("enabled"))
            c["rev"] += 1
            c["stage"] = "pending"
            c["apply_at"] = time.monotonic() + 0.15
            if self.fault == "dmr_resets_tetra" and c["mode"] == "dmr":
                t = self.channels["ch-tetra"]
                t["turns"] = 0
                t["phase"] = "pending"
                t["apply_at"] = time.monotonic() + 0.3
            if self.fault == "revision_bump" and c["mode"] == "p25":
                self.channels["ch-fm"]["rev"] += 1
            return 202, {"id": 4, "status": "accepted"}


def _serve(handler_cls):
    srv = ThreadingHTTPServer(("127.0.0.1", 0), handler_cls)
    srv.daemon_threads = True
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


def start(board=None):
    """(board, status server, dashboard server); .server_address[1] is the port."""
    b = board or Board()
    if not b.channels:
        b.setup_channels()

    class StatusHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *a):
            pass

        def _send(self, code, obj, ctype="application/json"):
            data = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            u = urllib.parse.urlsplit(self.path)
            q = dict(urllib.parse.parse_qsl(u.query, keep_blank_values=True))
            if u.path == "/metrics":
                self._send(200, b.metrics(q.get("full") == "1"))
            elif u.path == "/log":
                self._send(200, b.log_read(int(q.get("since", "0")), int(q.get("max", "16384"))))
            elif u.path == "/":
                self._send(200, b"Nexus-BS RTOS fake\n", "text/plain")
            elif u.path == "/cmd":
                self._cmd(q)
            elif u.path == "/__reboot":
                with b.lock:
                    b.boot = time.monotonic()
                    b.git = q.get("git", b.git)
                self._send(200, {"ok": True})
            else:
                self._send(404, {"error": "endpoints: / /metrics /log /cmd"})

        def do_POST(self):
            u = urllib.parse.urlsplit(self.path)
            n = int(self.headers.get("Content-Length", "0"))
            form = dict(urllib.parse.parse_qsl(self.rfile.read(n).decode()))
            if u.path == "/cmd":
                q = dict(urllib.parse.parse_qsl(u.query))
                q.update(form)
                self._cmd(q)
            else:
                self._send(404, {"error": "no"})

        def _cmd(self, q):
            auth = self.headers.get("Authorization", "")
            if auth != "Bearer " + TOKEN:
                self._send(401, {"error": "bad or missing bearer token"})
                return
            rc, out = b.cmd(q.get("c", ""))
            if rc < 0:
                self._send(403, {"error": "command not allowed"})
                return
            self._send(200, {"argv": q.get("c", "").split(), "rc": rc, "ms": 0.1, "output": out})

    class DashHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def _send(self, code, obj):
            data = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
            self.close_connection = True

        def _host_ok(self):
            # like dashboard_server.rs without auth: exactly one Host equal to ip:port
            hosts = self.headers.get_all("Host") or []
            return len(hosts) == 1 and hosts[0] == "%s:%d" % self.server.server_address

        def do_GET(self):
            if not self._host_ok():
                self.close_connection = True
                return
            if self.path == "/api/status":
                self._send(200, b.dash_status())
            else:
                self._send(404, {"error": "not_found"})

        def do_POST(self):
            if not self._host_ok():
                self.close_connection = True
                return
            n = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(n)
            if self.path != "/api/control/native":
                self._send(404, {"error": "not_found"})
                return
            origin_ok = self.headers.get("Origin") == "http://%s:%d" % self.server.server_address
            if not origin_ok or self.headers.get("X-Nexus2-Control") != "native":
                self._send(403, {"error": "control_origin_required"})
                return
            if self.headers.get("Content-Type") != "application/json":
                self._send(400, {"error": "invalid_request"})
                return
            code, receipt = b.dash_native(json.loads(body))
            self._send(code, receipt)

    return b, _serve(StatusHandler), _serve(DashHandler)
