#!/usr/bin/env python3
"""Generates ssh_secrets.h for rtos/app/ssh_server.c (build directory only, never committed).

Inputs
  --authorized-keys FILE   OpenSSH authorized_keys (ssh-ed25519 / ecdsa-sha2-nistp256 lines)
  env NEXUS_SSH_HOSTKEY    OpenSSH ed25519 private key (unencrypted). Empty: an ephemeral key
                           is generated with ssh-keygen for this build only.

The header holds the host key as OneAsymmetricKey DER (private seed + public key, the form
wolfSSH_CTX_UsePrivateKey_buffer(ASN1) needs), the authorized key blobs and a random per-build
seed salt. Nothing secret is printed, only the host key fingerprint. Passwords are not part
of the build: SSH checks the nexus2 dashboard credentials at run time.
"""
import argparse
import base64
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

# OneAsymmetricKey v2 as wolfCrypt writes and reads it (wc_Ed25519KeyToDer): the [1] public
# key carries the 32 raw key bytes (no BIT STRING unused-bits octet).
ED25519_OID_PREFIX = bytes.fromhex("3050020101300506032b657004220420")  # seed follows
ED25519_PUB_TAG = bytes.fromhex("8120")                                  # [1] public key


def ssh_string(buf, off):
    (n,) = struct.unpack(">I", buf[off:off + 4])
    off += 4
    if off + n > len(buf):
        raise ValueError("truncated SSH string")
    return buf[off:off + n], off + n


def parse_openssh_ed25519(text):
    """Returns (seed32, pub32, pubblob) of an unencrypted OpenSSH ed25519 private key."""
    lines = [l.strip() for l in text.strip().splitlines()]
    try:
        b = lines.index("-----BEGIN OPENSSH PRIVATE KEY-----")
        e = lines.index("-----END OPENSSH PRIVATE KEY-----")
    except ValueError:
        sys.exit("gen_secrets: NEXUS_SSH_HOSTKEY is not an OpenSSH private key")
    raw = base64.b64decode("".join(lines[b + 1:e]))
    magic = b"openssh-key-v1\0"
    if not raw.startswith(magic):
        sys.exit("gen_secrets: bad OpenSSH key magic")
    off = len(magic)
    cipher, off = ssh_string(raw, off)
    kdf, off = ssh_string(raw, off)
    _kdfopts, off = ssh_string(raw, off)
    if cipher != b"none" or kdf != b"none":
        sys.exit("gen_secrets: the host key must not be passphrase protected")
    (nkeys,) = struct.unpack(">I", raw[off:off + 4])
    off += 4
    if nkeys != 1:
        sys.exit("gen_secrets: expected exactly one key")
    pubblob, off = ssh_string(raw, off)
    priv, off = ssh_string(raw, off)
    c1, c2 = struct.unpack(">II", priv[:8])
    if c1 != c2:
        sys.exit("gen_secrets: corrupt private key (check bytes)")
    p = 8
    ktype, p = ssh_string(priv, p)
    if ktype != b"ssh-ed25519":
        sys.exit("gen_secrets: host key type %s, need ssh-ed25519" % ktype.decode(errors="replace"))
    pub, p = ssh_string(priv, p)
    sk, p = ssh_string(priv, p)
    if len(pub) != 32 or len(sk) != 64 or sk[32:] != pub:
        sys.exit("gen_secrets: malformed ed25519 key")
    return sk[:32], pub, pubblob


def ephemeral_key():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "hostkey")
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "nexus-rtos-ephemeral",
                        "-f", path], check=True, stdin=subprocess.DEVNULL)
        with open(path) as f:
            return f.read()


def fingerprint(pubblob):
    return "SHA256:" + base64.b64encode(hashlib.sha256(pubblob).digest()).decode().rstrip("=")


def authorized_keys(path):
    keys = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 2)
            if len(parts) < 2 or parts[0] not in ("ssh-ed25519", "ecdsa-sha2-nistp256"):
                print("gen_secrets: %s:%d: skipped (only plain ssh-ed25519 / "
                      "ecdsa-sha2-nistp256 lines)" % (path, n), file=sys.stderr)
                continue
            blob = base64.b64decode(parts[1])
            ktype, _ = ssh_string(blob, 0)
            if ktype.decode() != parts[0]:
                sys.exit("gen_secrets: %s:%d: key type mismatch" % (path, n))
            comment = parts[2] if len(parts) > 2 else "line %d" % n
            keys.append((blob, comment))
    return keys


def c_bytes(name, data):
    body = ",".join("0x%02x" % b for b in data)
    return ("static const unsigned char %s[%d] __attribute__((unused)) = {%s};\n"
            % (name, len(data), body))


def c_str(s):
    return '"' + "".join(c if c.isalnum() or c in " @._-:+/=" else "?" for c in s) + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--authorized-keys", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    hostkey = os.environ.get("NEXUS_SSH_HOSTKEY", "")
    ephemeral = not hostkey.strip()
    if ephemeral:
        hostkey = ephemeral_key()
    seed, pub, pubblob = parse_openssh_ed25519(hostkey)
    der = ED25519_OID_PREFIX + seed + ED25519_PUB_TAG + pub
    fp = fingerprint(pubblob)

    keys = authorized_keys(args.authorized_keys)

    out = ["/* generated by rtos/ssh/gen_secrets.py: secret, do not commit or publish */\n",
           "#ifndef NEXUS_SSH_SECRETS_H\n#define NEXUS_SSH_SECRETS_H\n",
           "#define NEXUS_SSH_HOSTKEY_EPHEMERAL %d\n" % (1 if ephemeral else 0),
           c_bytes("nexus_ssh_hostkey_der", der),
           "static const char nexus_ssh_hostkey_fp[] = %s;\n" % c_str(fp),
           c_bytes("nexus_ssh_seed_salt", os.urandom(32))]
    for i, (blob, _) in enumerate(keys):
        out.append(c_bytes("nexus_ssh_authkey_%d" % i, blob))
    out.append("struct nexus_ssh_authorized_key { const unsigned char *blob; unsigned int len; "
               "const char *comment; };\n")
    out.append("#define NEXUS_SSH_AUTHORIZED_COUNT %d\n" % len(keys))
    out.append("static const struct nexus_ssh_authorized_key nexus_ssh_authorized[%d] = {\n"
               % max(1, len(keys)))
    for i, (blob, comment) in enumerate(keys):
        out.append("  {nexus_ssh_authkey_%d, %d, %s},\n" % (i, len(blob), c_str(comment)))
    if not keys:
        out.append("  {0, 0, 0},\n")
    out.append("};\n#endif\n")

    fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write("".join(out))

    print("ssh: host key ed25519 %s%s" % (fp, " (ephemeral, generated for this build)"
                                          if ephemeral else ""))
    print("ssh: %d authorized key(s); passwords: nexus2 dashboard credentials" % len(keys))


if __name__ == "__main__":
    main()
