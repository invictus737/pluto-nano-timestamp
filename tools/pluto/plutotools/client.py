"""Clients: the board status server (port 80), the nexus2 dashboard API and SSH exec.

Board (rtos/app/http_status.c):
  GET /metrics[?top=N&full=1&hw=0]   JSON nexus.metrics/1
  GET /log?since=N[&max=B]           {since,start,next,oldest,gap,text}
  GET|POST /cmd?c=...                allowlisted shell command, Authorization: Bearer <token>

Dashboard (nexus2 crates/hotspot/src/dashboard_server.rs, default port 8080):
  GET /api/status                    channels[] with native_control
  POST /api/control/native           {"channel","enabled","expected_revision"}; needs
                                     X-Nexus2-Control: native, a same-origin Origin, JSON body
  Without dashboard credentials the server only answers a Host header equal to its own
  ip:port, so address the board by IP. With [dashboard.auth] pass user/password (Basic).
"""

import base64
import http.client
import json
import os
import subprocess
import urllib.parse

SCHEMA = "nexus.metrics/1"


class BoardError(Exception):
    """The board did not answer, or answered with an error."""

    def __init__(self, message, status=None, body=None):
        super().__init__(message)
        self.status = status
        self.body = body


def split_hostport(host, default_port):
    """'1.2.3.4' / '1.2.3.4:81' / 'http://1.2.3.4:81' -> ('1.2.3.4', 81)."""
    if "://" in host:
        u = urllib.parse.urlsplit(host)
        return u.hostname, u.port or default_port
    if host.count(":") == 1:
        h, p = host.split(":")
        return h, int(p)
    return host, default_port


def _request(host, port, method, path, body=None, headers=None, timeout=5.0):
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        hdrs = dict(headers or {})
        if body is not None and "Content-Length" not in hdrs:
            hdrs["Content-Length"] = str(len(body))
        conn.request(method, path, body=body, headers=hdrs)
        r = conn.getresponse()
        data = r.read()
        return r.status, r.reason, data
    except (OSError, http.client.HTTPException) as e:
        raise BoardError("%s %s:%d%s: %s" % (method, host, port, path, e)) from e
    finally:
        conn.close()


def check_schema(m):
    """Raise if the board speaks an incompatible metrics schema major."""
    schema = m.get("schema", "")
    want_name, want_major = SCHEMA.split("/")
    name, _, major = schema.partition("/")
    if name != want_name or major.split(".")[0] != want_major:
        raise BoardError("metrics schema %r, tools speak %s" % (schema, SCHEMA))
    return m


class Board:
    """The port 80 status server of the RTEMS image."""

    def __init__(self, host, token=None, timeout=5.0):
        self.host, self.port = split_hostport(host, 80)
        self.token = token
        self.timeout = timeout

    def _get_json(self, path, timeout=None):
        status, reason, data = _request(self.host, self.port, "GET", path, timeout=timeout or self.timeout)
        try:
            doc = json.loads(data.decode("utf-8"))
        except ValueError as e:
            raise BoardError("GET %s: not JSON (%s %s)" % (path, status, reason), status, data) from e
        if status != 200:
            raise BoardError("GET %s: %s %s" % (path, status, doc.get("error", reason)), status, doc)
        return doc

    def metrics(self, top=12, full=False, hw=True):
        q = {"top": top}
        if full:
            q["full"] = 1
        if not hw:
            q["hw"] = 0
        return check_schema(self._get_json("/metrics?" + urllib.parse.urlencode(q)))

    def log(self, since=0, max_bytes=16384):
        return self._get_json("/log?" + urllib.parse.urlencode({"since": since, "max": max_bytes}))

    def status_text(self):
        status, _, data = _request(self.host, self.port, "GET", "/", timeout=self.timeout)
        if status != 200:
            raise BoardError("GET /: %s" % status, status, data)
        return data.decode("utf-8", "replace")

    def cmd(self, command, timeout=20.0):
        """Run an allowlisted command; returns {"argv","rc","ms","output"}."""
        if not self.token:
            raise BoardError("/cmd needs the metrics_token (set PLUTO_TOKEN or --token-env)")
        body = urllib.parse.urlencode({"c": command}).encode()
        headers = {
            "Authorization": "Bearer " + self.token,
            "Content-Type": "application/x-www-form-urlencoded",
        }
        status, reason, data = _request(self.host, self.port, "POST", "/cmd", body, headers, timeout)
        try:
            doc = json.loads(data.decode("utf-8"))
        except ValueError as e:
            raise BoardError("/cmd: not JSON (%s %s)" % (status, reason), status, data) from e
        if status != 200:
            raise BoardError("/cmd %r: %s %s" % (command, status, doc.get("error", reason)), status, doc)
        return doc


def strip_telnet(text):
    """Drop telnet IAC sequences the RTEMS pty driver may emit."""
    out = bytearray()
    b = text.encode("latin-1", "replace") if isinstance(text, str) else bytes(text)
    i = 0
    while i < len(b):
        if b[i] == 0xFF and i + 1 < len(b):
            i += 3 if b[i + 1] in (0xFB, 0xFC, 0xFD, 0xFE) else 2
            continue
        out.append(b[i])
        i += 1
    return out.decode("utf-8", "replace").replace("\r\n", "\n")


class SSH:
    """`ssh user@board <command>` (the board's wolfSSH server runs exec requests in an RTEMS
    shell). Key authentication only (BatchMode), host key checked against known_hosts."""

    def __init__(self, host, user="root", key=None, known_hosts=None, port=22, connect_timeout=8):
        self.host = split_hostport(host, 80)[0]
        self.user = user
        self.key = key
        self.known_hosts = known_hosts
        self.port = port
        self.connect_timeout = connect_timeout

    def argv(self, command):
        a = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=%d" % self.connect_timeout,
             "-o", "ServerAliveInterval=5", "-p", str(self.port)]
        if self.known_hosts:
            a += ["-o", "UserKnownHostsFile=" + os.path.expanduser(self.known_hosts),
                  "-o", "StrictHostKeyChecking=yes"]
        if self.key:
            a += ["-i", os.path.expanduser(self.key), "-o", "IdentitiesOnly=yes"]
        a += ["%s@%s" % (self.user, self.host), command]
        return a

    def run(self, command, timeout=30):
        p = subprocess.run(self.argv(command), capture_output=True, timeout=timeout)
        return p.returncode, strip_telnet(p.stdout + p.stderr)

    def popen(self, command):
        return subprocess.Popen(self.argv(command), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL)


class CmdRunner:
    """Station commands over /cmd (token) or SSH exec, same allowlisted vocabulary."""

    def __init__(self, board=None, ssh=None):
        self.board = board
        self.ssh = ssh

    @property
    def available(self):
        return bool((self.board and self.board.token) or self.ssh)

    def run(self, command, timeout=20.0):
        if self.board is not None and self.board.token:
            return self.board.cmd(command, timeout=timeout)["output"]
        if self.ssh is not None:
            rc, out = self.ssh.run(command, timeout=timeout)
            if rc != 0 and not out:
                raise BoardError("ssh %r: exit %d" % (command, rc))
            return out
        raise BoardError("no command path: give a metrics_token (PLUTO_TOKEN) or --ssh-key")


class Dashboard:
    """nexus2 dashboard API."""

    def __init__(self, url, user=None, password=None, timeout=8.0):
        if "://" not in url:
            url = "http://" + url
        u = urllib.parse.urlsplit(url)
        self.host = u.hostname
        self.port = u.port or 8080
        self.timeout = timeout
        self.auth = None
        if user is not None and password is not None:
            self.auth = "Basic " + base64.b64encode(("%s:%s" % (user, password)).encode()).decode()

    @property
    def hostport(self):
        return "%s:%d" % (self.host, self.port)

    def _headers(self, extra=None):
        h = {"Host": self.hostport}
        if self.auth:
            h["Authorization"] = self.auth
        h.update(extra or {})
        return h

    def get(self, path):
        status, reason, data = _request(self.host, self.port, "GET", path, headers=self._headers(),
                                        timeout=self.timeout)
        if status == 401:
            raise BoardError("dashboard %s: 401 (credentials needed)" % path, status)
        if status != 200 or not data:
            raise BoardError("dashboard %s: %s %s (no answer usually means the Host header is not "
                             "the board's ip:port)" % (path, status, reason), status, data)
        return json.loads(data.decode("utf-8"))

    def status(self):
        return self.get("/api/status")

    def native(self, channel, enabled, revision):
        """POST /api/control/native; returns (http status, receipt dict)."""
        body = json.dumps({"channel": channel, "enabled": bool(enabled),
                           "expected_revision": str(revision)}).encode()
        h = self._headers({
            "Origin": "http://" + self.hostport,
            "Content-Type": "application/json",
            "X-Nexus2-Control": "native",
        })
        status, reason, data = _request(self.host, self.port, "POST", "/api/control/native", body, h,
                                        timeout=self.timeout + 10)
        try:
            receipt = json.loads(data.decode("utf-8")) if data else {}
        except ValueError:
            receipt = {"raw": data.decode("utf-8", "replace")}
        return status, receipt
