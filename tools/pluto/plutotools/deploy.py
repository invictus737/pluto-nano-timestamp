"""pluto-deploy: image -> (zopfli repack) -> fwupdate over TCP -> reboot -> wait for /metrics.

  pluto-deploy 192.168.1.160 out/rtos-nexus2-cherry.itb --ssh-key ~/.ssh/pluto --known-hosts kh
  pluto-deploy 192.168.1.160 rtos-ci-412                 CI draft release tag (gh), asset below
  pluto-deploy 192.168.1.160 rtos-ci-412 --asset rtos-nexus2.itb --reboot slcr --yes
  pluto-deploy - rtos-ci-412 --dry-run                   download + repack + checks only

Steps (each timed):
 1. image: a local .itb, or a draft release tag of the CI (`gh release download`, the release's
    target commit becomes the expected build id)
 2. repack: the gzip kernel recompressed with zopfli (pip module or `zopfli` command), all other
    FIT bytes unchanged, kernel hash updated, verified; skipped without zopfli or --no-repack
 3. `ssh root@board fwupdate <port> [reboot]` (key auth, host key from --known-hosts), wait for
    "waiting on TCP port", send the image to <port>, wait for "fwupdate: OK" (sectors whose
    content is unchanged are skipped by the board)
 4. reboot: fwupdate's own `reboot` argument (default), or `--reboot slcr` (SLCR unlock + PS
    software reset through mw32), or `--reboot none`
 5. wait for /metrics to answer again with a fresh uptime and print the build id; with an
    expected commit (tag) a mismatch exits 3

Flashing a live station needs the operator's OK: the tool asks unless --yes.

Serial fallback (not automatic): when the board does not come back, attach the console
(/dev/cu.usbserial-*, 115200 8N1), stop U-Boot, `loady` the .itb over YMODEM at 115200, then
`sf probe; sf update <addr> 0x200000 <size>` and `setenv fit_size <size>; saveenv` -- the
bring-up's serial_flash.py does exactly this; see tools/pluto/README.md.
"""

import argparse
import hashlib
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

from . import client, fit

REPO = "invictus737/pluto-nano-timestamp"
DEFAULT_ASSET = "rtos-nexus2-cherry.itb"


class DeployError(Exception):
    pass


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def resolve_image(src, asset, workdir, repo=REPO):
    """(path, expected commit or None)."""
    if os.path.isfile(src):
        return src, None
    if not re.match(r"^[A-Za-z0-9._-]+$", src):
        raise DeployError("%s: neither a file nor a release tag" % src)
    log("downloading %s from release %s (%s)" % (asset, src, repo))
    r = subprocess.run(["gh", "release", "download", src, "-R", repo, "-p", asset, "-D", workdir, "--clobber"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise DeployError("gh release download: %s" % (r.stderr.strip() or r.stdout.strip()))
    path = os.path.join(workdir, asset)
    commit = None
    v = subprocess.run(["gh", "release", "view", src, "-R", repo, "--json", "targetCommitish",
                        "-q", ".targetCommitish"], capture_output=True, text=True)
    if v.returncode == 0 and re.match(r"^[0-9a-f]{7,40}$", v.stdout.strip()):
        commit = v.stdout.strip()
    return path, commit


class FwSession:
    """The board side of fwupdate, through an SSH exec session whose output is followed."""

    def __init__(self, ssh, port, reboot):
        self.proc = ssh.popen("fwupdate %d%s" % (port, " reboot" if reboot else ""))
        self.lines = []
        self.cv = threading.Condition()
        self.done = False
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        for chunk in iter(lambda: self.proc.stdout.read1(4096), b""):
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = client.strip_telnet(line).strip()
                if text:
                    with self.cv:
                        self.lines.append(text)
                        self.cv.notify_all()
                    log("board: " + text)
        with self.cv:
            self.done = True
            self.cv.notify_all()

    def wait_for(self, patterns, fail_patterns, timeout):
        """First line matching any pattern (returns it); raises on a failure line or timeout."""
        end = time.monotonic() + timeout
        seen = 0
        with self.cv:
            while True:
                while seen < len(self.lines):
                    line = self.lines[seen]
                    seen += 1
                    if any(p in line for p in fail_patterns):
                        raise DeployError("board: " + line)
                    if any(p in line for p in patterns):
                        return line
                if self.done:
                    raise DeployError("ssh session ended (exit %s) before %r" % (self.proc.poll(), patterns))
                left = end - time.monotonic()
                if left <= 0:
                    raise DeployError("timeout waiting for %r" % (patterns,))
                self.cv.wait(min(left, 1.0))

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()


FAIL = ("nothing written", "FAILED", "failed", "not a FIT", "nobody connected", "no memory",
        "larger than", "only ", "cannot")


def send_image(host, port, data, timeout=60):
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall(data)
        s.shutdown(socket.SHUT_WR)
        s.settimeout(5)
        try:
            while s.recv(4096):
                pass
        except OSError:
            pass


def wait_board(board, old_uptime, timeout):
    """Wait for /metrics from a new boot: an uptime below the pre-flash one, or (unknown
    before) any answer after the board was seen down."""
    end = time.monotonic() + timeout
    down_seen = False
    while time.monotonic() < end:
        try:
            m = board.metrics(top=4, hw=False)
            up = m.get("uptime_s")
            if (old_uptime is not None and up is not None and up < old_uptime) or \
                    (old_uptime is None and down_seen):
                return m
        except client.BoardError:
            down_seen = True
        time.sleep(2)
    raise DeployError("board did not come back within %d s (serial fallback: see --help)" % timeout)


def build_matches(commit, board_git):
    """The board's build.git (12-char sha, maybe -dirty) names the expected commit."""
    if not commit or not board_git or board_git == "unknown":
        return True
    sha = board_git.split("-")[0]
    return len(sha) >= 7 and commit.startswith(sha)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="pluto-deploy", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board", help="board ip ('-' with --dry-run)")
    ap.add_argument("image", help="local .itb or CI release tag (rtos-ci-<run>)")
    ap.add_argument("--asset", default=DEFAULT_ASSET, help="release asset (default %(default)s)")
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--no-repack", action="store_true")
    ap.add_argument("--port", type=int, default=4444, help="fwupdate TCP port")
    ap.add_argument("--reboot", choices=("fwupdate", "slcr", "none"), default="fwupdate")
    ap.add_argument("--ssh-user", default="root")
    ap.add_argument("--ssh-key", default=os.environ.get("PLUTO_SSH_KEY"))
    ap.add_argument("--known-hosts", default=os.environ.get("PLUTO_KNOWN_HOSTS"),
                    help="known_hosts file holding the board's host key (StrictHostKeyChecking)")
    ap.add_argument("--flash-timeout", type=float, default=300)
    ap.add_argument("--boot-timeout", type=float, default=240)
    ap.add_argument("--out", help="keep the (repacked) image here")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--yes", action="store_true", help="do not ask before flashing")
    args = ap.parse_args(argv)

    t_all = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="pluto-deploy-") as wd:
        try:
            t = time.monotonic()
            path, commit = resolve_image(args.image, args.asset, wd, args.repo)
            data = open(path, "rb").read()
            fit.check_image(data)
            log("image %s: %d bytes%s (%.1f s)" % (path, len(data),
                                                    (", commit " + commit[:12]) if commit else "", time.monotonic() - t))
            if not args.no_repack:
                t = time.monotonic()
                data, note = fit.repack(data)
                log("repack: %s (%.1f s)" % (note, time.monotonic() - t))
            sha = hashlib.sha256(data).hexdigest()
            log("send: %d bytes (0x%X) sha256 %s" % (len(data), len(data), sha))
            if args.out:
                with open(args.out, "wb") as f:
                    f.write(data)
                log("kept as %s" % args.out)
            if args.dry_run:
                return 0
            if not args.ssh_key or not args.known_hosts:
                raise DeployError("--ssh-key and --known-hosts are required (or PLUTO_SSH_KEY / PLUTO_KNOWN_HOSTS)")
            board = client.Board(args.board)
            host = board.host
            old_uptime = None
            try:
                m = board.metrics(top=4, hw=False)
                old_uptime = m.get("uptime_s")
                log("board now: build %s, up %.0f s, nexus2 %s" % (
                    (m.get("build") or {}).get("git"), old_uptime or 0, (m.get("nexus2") or {}).get("state")))
            except client.BoardError as e:
                log("board /metrics not answering (%s): flashing anyway over SSH" % e)
            if not args.yes:
                ans = input("Flash %s with this image and reboot (%s)? [y/N] " % (host, args.reboot))
                if ans.strip().lower() not in ("y", "yes"):
                    log("not flashed")
                    return 1
            ssh = client.SSH(host, user=args.ssh_user, key=args.ssh_key, known_hosts=args.known_hosts)
            t = time.monotonic()
            sess = FwSession(ssh, args.port, args.reboot == "fwupdate")
            try:
                sess.wait_for(("waiting on TCP port",), FAIL, 20)
                send_image(host, args.port, data)
                log("sent in %.1f s" % (time.monotonic() - t))
                ok = sess.wait_for(("fwupdate: OK",), FAIL, args.flash_timeout)
                log("flashed in %.1f s: %s" % (time.monotonic() - t, ok))
            finally:
                if args.reboot != "fwupdate":
                    sess.close()
            if args.reboot == "none":
                log("not rebooted (--reboot none); total %.1f s" % (time.monotonic() - t_all))
                return 0
            if args.reboot == "slcr":
                # SLCR unlock, then PS_RST_CTRL soft reset: the second command never returns
                ssh.run("mw32 0xF8000008 0xDF0D", timeout=15)
                try:
                    ssh.run("mw32 0xF8000200 1", timeout=8)
                except subprocess.TimeoutExpired:
                    pass
            t = time.monotonic()
            m = wait_board(board, old_uptime, args.boot_timeout)
            sess.close()
            b = m.get("build") or {}
            log("board back in %.1f s: build %s (%s, %s), up %.1f s, nexus2 %s; total %.1f s" % (
                time.monotonic() - t, b.get("git"), b.get("image"), b.get("built"), m.get("uptime_s", 0),
                (m.get("nexus2") or {}).get("state"), time.monotonic() - t_all))
            if not build_matches(commit, b.get("git")):
                log("BUILD MISMATCH: expected %s, board runs %s" % (commit[:12], b.get("git")))
                return 3
            return 0
        except (DeployError, fit.FitError, client.BoardError) as e:
            log("FAILED: %s" % e)
            return 2


if __name__ == "__main__":
    sys.exit(main())
