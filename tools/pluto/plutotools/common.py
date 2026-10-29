"""CLI plumbing shared by the tools: board address, command path (token or SSH), sampling."""

import json
import os
import time

from . import client, metrics


def add_board_args(ap):
    ap.add_argument("board", nargs="?", default=os.environ.get("PLUTO_HOST"), help="board ip ($PLUTO_HOST)")
    ap.add_argument("--token-env", default="PLUTO_TOKEN",
                    help="environment variable holding the board's metrics_token (/cmd)")
    ap.add_argument("--ssh-key", default=os.environ.get("PLUTO_SSH_KEY"),
                    help="use SSH exec for commands (when no token)")
    ap.add_argument("--known-hosts", default=os.environ.get("PLUTO_KNOWN_HOSTS"))
    ap.add_argument("--ssh-user", default="root")


def board_from(args):
    if not args.board:
        raise SystemExit("board address required (argument or PLUTO_HOST)")
    token = os.environ.get(args.token_env) if args.token_env else None
    board = client.Board(args.board, token=token or None)
    ssh = None
    if args.ssh_key:
        if not args.known_hosts:
            raise SystemExit("--ssh-key needs --known-hosts (host key checking)")
        ssh = client.SSH(board.host, user=args.ssh_user, key=args.ssh_key, known_hosts=args.known_hosts)
    return board, client.CmdRunner(board, ssh)


class Recorder:
    """Optional JSONL of every poll: {host_t, tag, metrics|status|error}."""

    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8") if path else None

    def write(self, **kw):
        if self.f:
            kw.setdefault("host_t", time.time())
            self.f.write(json.dumps(kw) + "\n")
            self.f.flush()

    def close(self):
        if self.f:
            self.f.close()


def wait_state(board, want, timeout=60.0):
    """Wait until nexus2.state == want; returns the last metrics."""
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            last = board.metrics(top=4, hw=False)
            if (last.get("nexus2") or {}).get("state") == want:
                return last
        except client.BoardError:
            pass
        time.sleep(1.0)
    raise client.BoardError("nexus2 did not reach %r within %.0f s (last %r)" % (
        want, timeout, ((last or {}).get("nexus2") or {}).get("state")))


def sample(board, seconds, interval, rec=None, tag="", on_sample=None):
    """Poll /metrics for `seconds`; returns ([flat], poll errors)."""
    out, errors = [], 0
    end = time.monotonic() + seconds
    while True:
        t0 = time.monotonic()
        try:
            m = board.metrics(top=8)
            f = metrics.flatten(m)
            f["host_t"] = time.time()
            out.append(f)
            if rec:
                rec.write(tag=tag, metrics=m)
            if on_sample:
                on_sample(f)
        except client.BoardError as e:
            errors += 1
            if rec:
                rec.write(tag=tag, error=str(e))
        if t0 >= end:
            break
        # the last sleep ends at the window end: the next poll is the closing sample
        time.sleep(max(0.0, min(interval - (time.monotonic() - t0), end - time.monotonic())))
    return out, errors
