"""The four tools end to end against the fake board (tests/fakeboard.py)."""

import contextlib
import io
import json
import os
import shutil
import socket
import stat
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from plutotools import ab, client, deploy, modes, watch  # noqa: E402
import fakeboard  # noqa: E402
from test_lib import make_fit  # noqa: E402


def run(main, argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = main(argv)
    return rc, out.getvalue()


class Base(unittest.TestCase):
    def setUp(self):
        self.board, self.srv, self.dsrv = fakeboard.start()
        self.addr = "127.0.0.1:%d" % self.srv.server_address[1]
        self.dash = "http://127.0.0.1:%d" % self.dsrv.server_address[1]
        self.tmp = tempfile.mkdtemp(prefix="pluto-tools-")
        self.env = dict(os.environ)
        os.environ["PLUTO_TOKEN"] = fakeboard.TOKEN

    def tearDown(self):
        self.srv.shutdown()
        self.dsrv.shutdown()
        shutil.rmtree(self.tmp, ignore_errors=True)
        os.environ.clear()
        os.environ.update(self.env)


class Clients(Base):
    def test_board_endpoints(self):
        b = client.Board(self.addr, token=fakeboard.TOKEN)
        m = b.metrics(full=True)
        self.assertIn("stacks", m)
        self.assertEqual(b.cmd("nexus status")["output"], "nexus2: running, layout light\n")
        with self.assertRaises(client.BoardError) as e:
            client.Board(self.addr, token="wrong").cmd("nexus stop")
        self.assertEqual(e.exception.status, 401)
        with self.assertRaises(client.BoardError):
            client.Board(self.addr).cmd("nexus stop")  # no token: refused locally
        r = b.log(0)
        self.assertEqual(r["text"], "boot line 1\nboot line 2\n")
        self.board.append_log("x\n")
        self.assertEqual(b.log(r["next"])["text"], "x\n")
        self.assertIn("fake", b.status_text())

    def test_dashboard_host_rule(self):
        d = client.Dashboard(self.dash)
        self.assertEqual(len(d.status()["channels"]), 4)
        # a name instead of the ip:port gets no answer (like nexus2 without credentials)
        bad = client.Dashboard(self.dash.replace("127.0.0.1", "localhost"))
        with self.assertRaises(client.BoardError):
            bad.status()

    def test_telnet_strip(self):
        self.assertEqual(client.strip_telnet(b"\xff\xfb\x01ok\r\n\xff\xf1x"), "ok\nx")


class Watch(Base):
    def test_once_ok(self):
        rc, out = run(watch.main, [self.addr, "--once", "--no-color"])
        self.assertEqual(rc, 0, out)
        for s in ("CPU0", "CPU1", "drive.turn", "tx.late", "AD9363", "USB", "wifi Chris", "budget OK", "STACKS"):
            self.assertIn(s, out)

    def test_flags_and_record(self):
        self.board.late_per_poll = 1
        rec = os.path.join(self.tmp, "w.jsonl")
        logf = os.path.join(self.tmp, "w.log")
        rc, out = run(watch.main, [self.addr, "--count", "3", "-i", "0.05", "--no-color", "--record", rec,
                                   "--log-file", logf])
        self.assertEqual(rc, 1)
        self.assertIn("tx_late +1", out)
        lines = [json.loads(x) for x in open(rec)]
        self.assertEqual(len(lines), 3)
        self.assertEqual(lines[0]["metrics"]["schema"], "nexus.metrics/1")

    def test_unreachable(self):
        port = _free_port()
        rc, out = run(watch.main, ["127.0.0.1:%d" % port, "--once", "--no-color"])
        self.assertEqual(rc, 2)
        self.assertIn("!!", out)


class AB(Base):
    def test_env_ab(self):
        out_json = os.path.join(self.tmp, "ab.json")
        rc, out = run(ab.main, [self.addr, "--minutes", "0.02", "--warmup", "0", "--interval", "0.2",
                                "--a-label", "fast", "--a-env", "NEXUS2_SLOW=0",
                                "--b-label", "slow", "--b-env", "NEXUS2_SLOW=1", "--out", out_json])
        self.assertEqual(rc, 1, out)  # slow breaks the CPU1 budget
        rep = json.load(open(out_json))
        self.assertEqual(rep["verdict"]["fast"]["verdict"], "PASS")
        self.assertEqual(rep["verdict"]["slow"]["verdict"], "FAIL")
        self.assertTrue(any("deadline CPU1" in r for r in rep["verdict"]["slow"]["reasons"]))
        self.assertIn("cpu1 busy mean %", out)
        self.assertIn("rx.analysis mean us", out)
        cmds = self.board.cmds
        for c in ("nexus stop", "nexus env NEXUS2_SLOW=1", "nexus start", "nexus stages reset"):
            self.assertIn(c, cmds)

    def test_tx_late_fails(self):
        self.board.late_per_poll = 1
        rc, out = run(ab.main, [self.addr, "--minutes", "0.01", "--warmup", "0", "--interval", "0.2",
                                "--a-layout", "light", "--b-layout", "cpu1"])
        self.assertEqual(rc, 1)
        self.assertIn("tx.late +", out)
        self.assertIn("nexus layout cpu1", self.board.cmds)


class Modes(Base):
    ARGS = ["--settle", "0", "--window", "0.4", "--interval", "0.2", "--cycles", "2", "--hold", "0.25",
            "--sample", "0.1"]

    def test_matrix_and_isolation_pass(self):
        out_json = os.path.join(self.tmp, "modes.json")
        rc, out = run(modes.main, [self.addr, "--dash", self.dash, "--out", out_json] + self.ARGS)
        self.assertEqual(rc, 0, out)
        rep = json.load(open(out_json))
        self.assertEqual([r["modes"] for r in rep["cost"]],
                         [["tetra"], ["tetra", "dmr"], ["tetra", "dmr", "p25"], ["tetra", "dmr", "p25", "fm"]])
        self.assertGreater(rep["cost"][-1]["cpu1"], rep["cost"][0]["cpu1"])
        self.assertEqual([v["mode"] for v in rep["isolation"]], ["dmr", "p25", "fm"])
        self.assertTrue(all(v["verdict"] == "PASS" and v["idempotent"] for v in rep["isolation"]), out)
        # initial states restored
        self.assertTrue(all(c["desired"] for c in self.board.channels.values()))

    def test_disturbed_tetra_detected(self):
        self.board.fault = "dmr_resets_tetra"
        out_json = os.path.join(self.tmp, "modes.json")
        rc, out = run(modes.main, [self.addr, "--dash", self.dash, "--skip-cost", "--out", out_json] + self.ARGS)
        self.assertEqual(rc, 1, out)
        rep = {v["mode"]: v for v in json.load(open(out_json))["isolation"]}
        self.assertEqual(rep["dmr"]["verdict"], "FAIL")
        self.assertGreater(rep["dmr"]["stalls"] + rep["dmr"]["phase_changes"], 0)
        self.assertEqual(rep["fm"]["verdict"], "PASS")

    def test_revision_side_effect_detected(self):
        self.board.fault = "revision_bump"
        out_json = os.path.join(self.tmp, "modes.json")
        rc, out = run(modes.main, [self.addr, "--dash", self.dash, "--skip-cost", "--modes", "p25",
                                   "--out", out_json] + self.ARGS)
        self.assertEqual(rc, 1, out)
        v = json.load(open(out_json))["isolation"][0]
        self.assertGreater(v["revision_changes"], 0)


FAKE_SSH = r'''#!/usr/bin/env python3
import os, socket, sys, urllib.request
cmd = sys.argv[-1].split()
board = os.environ["FAKE_BOARD_URL"]
def say(s):
    print(s, flush=True)
if cmd[0] == "fwupdate":
    port = int(cmd[1])
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port)); s.listen(1)
    say("fwupdate: waiting on TCP port %d (120 s)" % port)
    c, _ = s.accept(); data = b""
    while True:
        b = c.recv(65536)
        if not b: break
        data += b
    c.close()
    if data[:4] != b"\xd0\x0d\xfe\xed":
        say("fwupdate: not a FIT image for the slot: nothing written"); sys.exit(1)
    open(os.environ["FAKE_FLASH_OUT"], "wb").write(data)
    say("fwupdate: %d bytes received, writing to QSPI 0x200000" % len(data))
    say("fwupdate: 3 sectors written, 1 unchanged")
    say("fwupdate: OK, fit_size %X%s" % (len(data), ", rebooting" if "reboot" in cmd else "; reboot to use it"))
    if "reboot" in cmd:
        urllib.request.urlopen(board + "/__reboot?git=" + os.environ.get("FAKE_NEW_GIT", "")).read()
elif cmd[0] == "mw32":
    say("0x%s <- %s" % (cmd[1], cmd[2]))
    if cmd[1].lower() == "0xf8000200":
        urllib.request.urlopen(board + "/__reboot?git=" + os.environ.get("FAKE_NEW_GIT", "")).read()
'''

FAKE_ZOPFLI = r'''#!/usr/bin/env python3
import gzip, sys
sys.stdout.buffer.write(gzip.compress(open(sys.argv[-1], "rb").read(), 9, mtime=0))
'''


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Deploy(Base):
    def setUp(self):
        super().setUp()
        self.board.boot -= 1000  # an uptime the new boot will be below
        bindir = os.path.join(self.tmp, "bin")
        os.mkdir(bindir)
        for name, text in (("ssh", FAKE_SSH), ("zopfli", FAKE_ZOPFLI)):
            p = os.path.join(bindir, name)
            with open(p, "w") as f:
                f.write(text)
            os.chmod(p, os.stat(p).st_mode | stat.S_IEXEC)
        os.environ["PATH"] = bindir + os.pathsep + os.environ["PATH"]
        os.environ["FAKE_BOARD_URL"] = "http://" + self.addr
        os.environ["FAKE_FLASH_OUT"] = os.path.join(self.tmp, "flashed.itb")
        os.environ["FAKE_NEW_GIT"] = "abcdef123456"
        self.image = os.path.join(self.tmp, "rtos-nexus2-cherry.itb")
        with open(self.image, "wb") as f:
            f.write(make_fit((b"kernel " * 30000) + os.urandom(2000), level=1))
        self.kh = os.path.join(self.tmp, "known_hosts")
        open(self.kh, "w").close()

    def common(self, *extra):
        return [self.addr, self.image, "--ssh-key", "/dev/null", "--known-hosts", self.kh, "--yes",
                "--port", str(_free_port()), "--boot-timeout", "20"] + list(extra)

    def test_flash_repack_reboot(self):
        rc, out = run(deploy.main, self.common())
        self.assertEqual(rc, 0, out)
        sent = open(os.environ["FAKE_FLASH_OUT"], "rb").read()
        self.assertLess(len(sent), os.path.getsize(self.image))  # zopfli stand-in made it smaller
        self.assertIn("fwupdate: OK", out)
        self.assertIn("board back", out)
        self.assertIn("abcdef123456", out)

    def test_slcr_reboot_no_repack(self):
        rc, out = run(deploy.main, self.common("--reboot", "slcr", "--no-repack"))
        self.assertEqual(rc, 0, out)
        self.assertEqual(open(os.environ["FAKE_FLASH_OUT"], "rb").read(), open(self.image, "rb").read())

    def test_dry_run_and_errors(self):
        keep = os.path.join(self.tmp, "kept.itb")
        rc, out = run(deploy.main, ["-", self.image, "--dry-run", "--out", keep])
        self.assertEqual(rc, 0, out)
        self.assertTrue(os.path.getsize(keep) > 0)
        rc, out = run(deploy.main, ["-", "no/such file", "--dry-run"])
        self.assertEqual(rc, 2)
        bad = os.path.join(self.tmp, "bad.itb")
        open(bad, "wb").write(b"junk" * 100)
        rc, out = run(deploy.main, ["-", bad, "--dry-run"])
        self.assertEqual(rc, 2)

    def test_build_match(self):
        self.assertTrue(deploy.build_matches(None, "abc"))
        self.assertTrue(deploy.build_matches("1448c76aaaaa0000", "1448c76aaaaa-dirty"))
        self.assertFalse(deploy.build_matches("1448c76aaaaa0000", "deadbeef0000"))
        self.assertTrue(deploy.build_matches("1448c76", "unknown"))


if __name__ == "__main__":
    unittest.main()
