"""rtos/app/metrics_fmt.c on the host: JSON writer, stage table parser, log text escaping and
the /cmd allowlist (built with the host cc and sanitizers when available)."""

import json
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.normpath(os.path.join(HERE, "..", "..", "..", "rtos", "app"))

STAGES = """stage                   count   mean_us   p99_us   max_us   budget     over     us/s
drive.turn              12345     410.2      512     2048     1000        3    41022
rx.analysis             99999      88.0      128      700     1250        0    17600
carrier0.tetr            5000      55.5       64      300        0        0     5550
carrier1.dmr             5000      21.0       32       90        0        0     2100
tx.lead                  4000    9000.0    16384    16000        0        0   900000
tx.late 7 blocks skipped since start
us/s = microseconds of CPU per second of wall time (1000000 = one full core); drive.wake and tx.lead are latencies, not CPU
(reset)
"""


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class BoardFmt(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="pluto-fmt-")
        cls.exe = os.path.join(cls.tmp, "fmt_driver")
        base = ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I" + APP,
                os.path.join(HERE, "fmt_driver.c"), os.path.join(APP, "metrics_fmt.c"), "-o", cls.exe, "-lm"]
        r = subprocess.run(base[:1] + ["-fsanitize=address,undefined"] + base[1:], capture_output=True, text=True)
        if r.returncode != 0:  # no sanitizer runtime: plain build
            r = subprocess.run(base, capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(r.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_driver(self, *args, data=None):
        path = None
        if data is not None:
            path = os.path.join(self.tmp, "in.bin")
            with open(path, "wb") as f:
                f.write(data)
            args = args + (path,)
        r = subprocess.run([self.exe] + list(args), capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        return r.stdout

    def test_document(self):
        d = json.loads(self.run_driver("doc"))
        self.assertEqual(d["schema"], "nexus.metrics/1")
        self.assertEqual(d["quote"], 'a"b\\c\n\t\x01')
        self.assertIsNone(d["none"])
        self.assertIsNone(d["nan"])
        self.assertEqual(d["big"], 18446744073709551615)
        self.assertEqual(d["pi"], 3.14)
        self.assertEqual(d["arr"], [1, {"empty": []}, "x"])
        self.assertEqual(d["empty_obj"], {})

    def test_overflow_gives_nothing(self):
        n, first = self.run_driver("overflow").split()
        self.assertEqual((n, first), (b"0", b"0"))

    def test_stage_table(self):
        d = json.loads(self.run_driver("stages", data=STAGES.encode()))
        self.assertEqual(d["rows"], 5)
        self.assertEqual(d["tx_late"], 7)
        names = [i["name"] for i in d["items"]]
        self.assertEqual(names, ["drive.turn", "rx.analysis", "carrier0.tetr", "carrier1.dmr", "tx.lead"])
        dt = d["items"][0]
        self.assertEqual((dt["count"], dt["mean_us"], dt["p99_us"], dt["max_us"], dt["budget_us"], dt["over"],
                          dt["us_per_s"]), (12345, 410.2, 512, 2048, 1000, 3, 41022))

    def test_stage_table_empty(self):
        d = json.loads(self.run_driver("stages", data=b"stage count\n"))
        self.assertEqual(d["items"], [])
        self.assertIsNone(d["tx_late"])

    def test_log_text_escaping(self):
        raw = b'ok "q" \\ \xff\xfe torn \xe2\x82 euro \xe2\x82\xac nul\x00 \x1b[31m end\n'
        d = json.loads(self.run_driver("text", data=raw))
        self.assertEqual(d["text"], 'ok "q" \\ �� torn �� euro € nul\x00 \x1b[31m end\n')
        # 6x worst case: every byte escaped
        d = json.loads(self.run_driver("text", data=bytes(range(32)) * 64))
        self.assertEqual(len(d["text"]), 32 * 64)

    def test_cmd_allowlist(self):
        cases = {
            "nexus start": 1, "nexus stop": 1, "nexus status": 1, "nexus layout": 1, "nexus layout light": 1,
            "nexus layout cpu1": 1, "nexus layout evil": 0, "nexus env": 1, "nexus env pluto": 1,
            "nexus env save": 1, "nexus env NEXUS2_DRIVE_WAKE=1 NEXUS2_TX_BATCH=4": 1,
            "nexus env RUST_LOG=info": 1, "nexus env HOME=/": 0, "nexus env NEXUS2_X": 0,
            "nexus stages": 1, "nexus stages reset": 1, "nexus stages drop": 0, "nexus load": 1,
            "nexus load 10": 1, "nexus load 11": 0, "nexus load 0": 0, "nexus load 5x": 0,
            "usbh": 1, "usbh reset": 0, "lsusb": 1, "lsusb -t": 1, "lsusb -v": 0, "net status": 1,
            "net down wlan0": 0, "wifi status": 1, "wifi connect x y": 0, "fwupdate 4444": 0,
            "mw32 0xF8000200 1": 0, "rm -r /data": 0, "config show": 0, "nexus": 0, "log": 0,
            "nexus env NEXUS2_A=1;reboot": 0, "nexus env NEXUS2_A=$(x)": 0, "nexus start extra": 0,
        }
        lines = list(cases)
        out = self.run_driver("allow", data=("\n".join(lines) + "\n").encode()).split()
        got = dict(zip(lines, (int(x) for x in out)))
        self.assertEqual(got, cases)


if __name__ == "__main__":
    unittest.main()
