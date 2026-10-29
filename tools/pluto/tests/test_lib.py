"""Pure-Python parts: metrics flattening and budget flags, statistics, FIT rebuild/repack."""

import gzip
import hashlib
import os
import struct
import sys
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from plutotools import client, fit, metrics, stats  # noqa: E402
import fakeboard  # noqa: E402


class Metrics(unittest.TestCase):
    def doc(self, **kw):
        b = fakeboard.Board()
        b.setup_channels()
        for k, v in kw.items():
            setattr(b, k, v)
        return b, b.metrics(full=True)

    def test_schema(self):
        _, m = self.doc()
        client.check_schema(m)
        with self.assertRaises(client.BoardError):
            client.check_schema(dict(m, schema="nexus.metrics/2"))
        with self.assertRaises(client.BoardError):
            client.check_schema({"schema": "other/1"})
        client.check_schema(dict(m, schema="nexus.metrics/1.3"))

    def test_flatten(self):
        _, m = self.doc()
        f = metrics.flatten(m)
        self.assertEqual(f["cpu0"], 22.5)
        self.assertIn("stage.drive.turn.mean_us", f)
        self.assertEqual(f["tx_late"], 0)
        self.assertEqual(f["usb.handshake_misses"], 0)
        self.assertNotIn("usb.gate", f)  # booleans are not counters
        self.assertTrue(f["wifi.connected"])
        self.assertEqual(f["net.default_route"], "192.168.1.1 via wlan0")
        self.assertEqual(f["nexus2.state"], "running")
        self.assertEqual(metrics.carrier_stage(f, "tetra"), "carrier0.tetr")
        self.assertEqual(metrics.carrier_stage(f, "dmr"), "carrier1.dmr")
        self.assertIsNone(metrics.carrier_stage(f, "nxdn"))

    def test_flatten_base_image(self):
        # a base image: no stages, no AD9363, libbsd USB
        f = metrics.flatten({"schema": "nexus.metrics/1", "uptime_s": 5, "stages": None, "ad9363": None,
                             "usb": None, "net": {"wifi": None, "ifaces": [], "default_route": None}})
        self.assertNotIn("tx_late", f)
        self.assertIsNone(f["net.default_route"])

    def test_stage_window(self):
        a = {"uptime_s": 10.0, "stage.x.count": 100, "stage.x.mean_us": 10.0}
        b = {"uptime_s": 12.0, "stage.x.count": 300, "stage.x.mean_us": 20.0}
        mean, usps = metrics.stage_window(a, b, "x")
        self.assertAlmostEqual(mean, (300 * 20 - 100 * 10) / 200.0)
        self.assertAlmostEqual(usps, (300 * 20 - 100 * 10) / 2.0)
        # reset between the samples: only what came after it
        c = {"uptime_s": 13.0, "stage.x.count": 50, "stage.x.mean_us": 4.0}
        mean, _ = metrics.stage_window(b, c, "x")
        self.assertAlmostEqual(mean, 4.0)
        self.assertEqual(metrics.stage_window(a, a, "x"), (None, 0.0))
        self.assertEqual(metrics.stage_window(a, b, "missing"), (None, None))

    def test_flags(self):
        budget = metrics.Budget()
        b, m0 = self.doc()
        f0 = metrics.flatten(m0)
        self.assertEqual(metrics.check_sample(None, f0, budget), [])
        b.tx_late, b.handshake, b.wifi = 2, 1, False
        b.env["NEXUS2_SLOW"] = "1"
        f1 = metrics.flatten(b.metrics(False))
        keys = {k for _, k, _ in metrics.check_sample(f0, f1, budget)}
        self.assertTrue({"cpu1", "tx_late", "usb.handshake_misses", "wifi.connected"} <= keys, keys)
        f1["uptime_s"] = 100.0
        f2 = dict(f1, uptime_s=1.0)
        self.assertIn("uptime_s", {k for _, k, _ in metrics.check_sample(f1, f2, budget)})
        f3 = dict(f1, **{"nexus2.state": "stopped"})
        self.assertIn("nexus2.state", {k for _, k, _ in metrics.check_sample(f1, f3, budget)})
        nowifi = metrics.Budget(require_wifi=False)
        self.assertNotIn("wifi.connected", {k for _, k, _ in metrics.check_sample(f0, f1, nowifi)})

    def test_stats(self):
        self.assertEqual(stats.pctl([1, 2, 3, 4], 50), 2)
        self.assertEqual(stats.pctl(list(range(1, 101)), 99), 99)
        self.assertIsNone(stats.pctl([], 50))
        s = stats.summary([1, None, 3])
        self.assertEqual((s["n"], s["mean"], s["max"]), (2, 2.0, 3))
        self.assertEqual(stats.fmt(None), "-")
        self.assertEqual(stats.fmt(2), "2")
        self.assertEqual(stats.pct_change(50, 75), 50.0)


def make_fit(kernel, level=1, algo="md5", extra_hash=None):
    """A FIT like rtos/ci/pack.sh's: stub fdt, fpga, gzip kernel with a hash node."""
    kz = gzip.compress(kernel, compresslevel=level, mtime=0)

    def h(a, d):
        return struct.pack(">I", zlib.crc32(d)) if a == "crc32" else hashlib.new(a, d).digest()

    items = [("begin", ""), ("prop", "/", "description", b"Nexus-BS RTOS (RTEMS) for Pluto Nano\0"),
             ("prop", "/", "#address-cells", struct.pack(">I", 1)),
             ("begin", "images"),
             ("begin", "fdt-1"), ("prop", "/images/fdt-1", "data", b"\xd0\x0d\xfe\xed" + b"\0" * 36),
             ("prop", "/images/fdt-1", "type", b"flat_dt\0"), ("end",),
             ("begin", "fpga-1"), ("prop", "/images/fpga-1", "data", os.urandom(1001)),
             ("prop", "/images/fpga-1", "load", struct.pack(">I", 0xF000000)), ("end",),
             ("begin", "rtems-1"), ("prop", "/images/rtems-1", "data", kz),
             ("prop", "/images/rtems-1", "compression", b"gzip\0"),
             ("prop", "/images/rtems-1", "load", struct.pack(">I", 0x104000)),
             ("begin", "hash-1"), ("prop", "/images/rtems-1/hash-1", "value", h(algo, kz)),
             ("prop", "/images/rtems-1/hash-1", "algo", algo.encode() + b"\0"), ("end",)]
    if extra_hash:
        items += [("begin", "hash-2"), ("prop", "/images/rtems-1/hash-2", "value", h(extra_hash, kz)),
                  ("prop", "/images/rtems-1/hash-2", "algo", extra_hash.encode() + b"\0"), ("end",)]
    items += [("end",), ("end",),
              ("begin", "configurations"), ("prop", "/configurations", "default", b"config-1\0"),
              ("begin", "config-1"), ("prop", "/configurations/config-1", "kernel", b"rtems-1\0"), ("end",),
              ("end",), ("end",)]
    return fit.build({"items": items, "rsv": b"\0" * 16, "version": 17})


class Fit(unittest.TestCase):
    kernel = (b"RTEMS kernel " * 4000) + os.urandom(3000) + b"\0" * 20000

    def test_roundtrip(self):
        d = make_fit(self.kernel)
        t = fit.parse(d)
        self.assertEqual(fit.build(t), d)
        tree, pr = fit.check_image(d)
        self.assertEqual(gzip.decompress(pr[(fit.KERNEL, "data")]), self.kernel)

    def test_repack_smaller_and_verified(self):
        d = make_fit(self.kernel, level=1, algo="md5", extra_hash="crc32")
        out, note = fit.repack(d, compressor=lambda raw: gzip.compress(raw, compresslevel=9, mtime=0))
        self.assertLess(len(out), len(d), note)
        pr0, pr1 = fit.props(fit.parse(d)), fit.props(fit.parse(out))
        self.assertEqual(set(pr0), set(pr1))
        kz = pr1[(fit.KERNEL, "data")]
        self.assertEqual(gzip.decompress(kz), self.kernel)
        self.assertEqual(pr1[(fit.KERNEL + "/hash-1", "value")], hashlib.md5(kz).digest())
        self.assertEqual(pr1[(fit.KERNEL + "/hash-2", "value")], struct.pack(">I", zlib.crc32(kz)))
        for k in pr0:
            if k[0] != fit.KERNEL and not k[0].startswith(fit.KERNEL + "/hash"):
                self.assertEqual(pr0[k], pr1[k], k)

    def test_repack_skips(self):
        d = make_fit(self.kernel, level=9)
        out, note = fit.repack(d, compressor=lambda raw: None)
        self.assertIs(out, d)
        self.assertIn("left as built", note)
        out, note = fit.repack(d, compressor=lambda raw: gzip.compress(raw, compresslevel=1, mtime=0))
        self.assertIs(out, d)
        with self.assertRaises(fit.FitError):
            fit.repack(d, compressor=lambda raw: gzip.compress(raw[:-1], compresslevel=9))

    def test_bad_images(self):
        with self.assertRaises(fit.FitError):
            fit.check_image(b"\0" * 100)
        with self.assertRaises(fit.FitError):
            fit.parse(make_fit(self.kernel)[:-500])


if __name__ == "__main__":
    unittest.main()
