"""FIT (flattened device tree) parsing, rebuilding and the zopfli kernel repack.

The CI image (rtos/ci/pack.sh) stores the RTEMS binary gzip -9 compressed in /images/rtems-1.
Recompressing the same bytes with zopfli makes the FIT smaller (fewer QSPI sectors to write and
less to send); everything else in the FIT stays byte-identical, the kernel hash is updated and
the result is verified (repack.py of the RTOS bring-up, without dtc: the FDT is re-serialised
here). zopfli comes from the optional Python module or the `zopfli` command; without either the
image is left as built.
"""

import gzip
import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
import zlib

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9
FIT_SLOT_MAX = 0x1E00000  # QSPI slot 0x200000..0x2000000 (rtos/QSPI.md)
KERNEL = "/images/rtems-1"


class FitError(Exception):
    pass


def _align4(n):
    return (n + 3) & ~3


def parse(b):
    """[(node path, prop name, value bytes)] in tree order, plus the reserve map bytes."""
    if len(b) < 40:
        raise FitError("not an FDT: %d bytes" % len(b))
    magic, total, off_struct, off_strings, off_rsv, version = struct.unpack(">IIIIII", b[:24])
    if magic != FDT_MAGIC:
        raise FitError("not an FDT: magic %08x" % magic)
    if total > len(b):
        raise FitError("FDT truncated: totalsize %d, have %d" % (total, len(b)))
    rsv = bytearray()
    p = off_rsv
    while True:
        addr, size = struct.unpack(">QQ", b[p:p + 16])
        rsv += b[p:p + 16]
        p += 16
        if addr == 0 and size == 0:
            break

    def string(o):
        e = b.index(b"\0", off_strings + o)
        return b[off_strings + o:e].decode()

    p = off_struct
    path = []
    items = []  # ("begin", name) | ("end",) | ("prop", path, name, value)
    while True:
        (tok,) = struct.unpack(">I", b[p:p + 4])
        p += 4
        if tok == FDT_BEGIN_NODE:
            e = b.index(b"\0", p)
            name = b[p:e].decode()
            path.append(name)
            items.append(("begin", name))
            p = _align4(e + 1)
        elif tok == FDT_END_NODE:
            path.pop()
            items.append(("end",))
        elif tok == FDT_PROP:
            ln, nameoff = struct.unpack(">II", b[p:p + 8])
            p += 8
            items.append(("prop", "/".join(path) or "/", string(nameoff), bytes(b[p:p + ln])))
            p = _align4(p + ln)
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            break
        else:
            raise FitError("bad FDT token %d at %d" % (tok, p - 4))
    return {"items": items, "rsv": bytes(rsv), "version": version}


def props(tree):
    """{(path, name): value}"""
    return {(it[1], it[2]): it[3] for it in tree["items"] if it[0] == "prop"}


def build(tree, replace=None):
    """Serialise a parsed tree (version 17 header), with {(path, name): new value} replaced."""
    replace = replace or {}
    strings = bytearray()
    str_off = {}
    st = bytearray()
    for it in tree["items"]:
        if it[0] == "begin":
            st += struct.pack(">I", FDT_BEGIN_NODE)
            n = it[1].encode() + b"\0"
            st += n + b"\0" * (_align4(len(n)) - len(n))
        elif it[0] == "end":
            st += struct.pack(">I", FDT_END_NODE)
        else:
            _, path, name, value = it
            value = replace.get((path, name), value)
            if name not in str_off:
                str_off[name] = len(strings)
                strings += name.encode() + b"\0"
            st += struct.pack(">III", FDT_PROP, len(value), str_off[name])
            st += value + b"\0" * (_align4(len(value)) - len(value))
    st += struct.pack(">I", FDT_END)
    hdr = 40
    off_rsv = (hdr + 7) & ~7
    off_struct = off_rsv + len(tree["rsv"])
    off_strings = off_struct + len(st)
    total = off_strings + len(strings)
    out = bytearray(struct.pack(">IIIIIIIIII", FDT_MAGIC, total, off_struct, off_strings, off_rsv,
                                17, 16, 0, len(strings), len(st)))
    out += b"\0" * (off_rsv - len(out))
    out += tree["rsv"] + st + strings
    return bytes(out)


def _hash(algo, data):
    algo = algo.rstrip(b"\0").decode()
    if algo == "crc32":
        return struct.pack(">I", zlib.crc32(data) & 0xFFFFFFFF)
    if algo in ("md5", "sha1", "sha256", "sha384", "sha512"):
        return hashlib.new(algo, data).digest()
    raise FitError("unsupported hash algo %r" % algo)


def check_image(data):
    """Basic FIT sanity before sending: FDT magic, size within the QSPI slot, kernel present."""
    tree = parse(data)
    pr = props(tree)
    if (KERNEL, "data") not in pr:
        raise FitError("no %s/data in the FIT" % KERNEL)
    if len(data) > FIT_SLOT_MAX:
        raise FitError("FIT %d bytes > slot %d" % (len(data), FIT_SLOT_MAX))
    return tree, pr


def zopfli_compress(raw, iterations=5):
    """gzip stream of raw by zopfli (module, then command); None if neither exists."""
    try:
        import zopfli.gzip as zg  # optional, not stdlib
        return zg.compress(raw, numiterations=iterations)
    except ImportError:
        pass
    exe = shutil.which("zopfli")
    if exe is None:
        return None
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "kernel")
        with open(p, "wb") as f:
            f.write(raw)
        r = subprocess.run([exe, "--i%d" % iterations, "-c", p], capture_output=True, check=True)
        return r.stdout


def repack(data, compressor=zopfli_compress):
    """(new FIT bytes, note). The kernel is recompressed; every other property is unchanged and
    the result is verified. Returns the input unchanged when no zopfli exists or it is not
    smaller."""
    tree, pr = check_image(data)
    kz = pr[(KERNEL, "data")]
    comp = pr.get((KERNEL, "compression"), b"none\0").rstrip(b"\0").decode()
    if comp != "gzip":
        return data, "kernel compression %s: left as built" % comp
    kernel = gzip.decompress(kz)
    nz = compressor(kernel)
    if nz is None:
        return data, "no zopfli (pip module or command): left as built"
    if gzip.decompress(nz) != kernel:
        raise FitError("recompressed kernel does not decompress to the original")
    if len(nz) >= len(kz):
        return data, "zopfli not smaller: left as built"
    rep = {(KERNEL, "data"): nz}
    hashes = []
    for (path, name), v in pr.items():
        if path.startswith(KERNEL + "/hash") and name == "value":
            algo = pr.get((path, "algo"))
            if algo is None:
                raise FitError("%s has no algo" % path)
            rep[(path, "value")] = _hash(algo, nz)
            hashes.append(path)
    out = build(tree, rep)
    # verify: same properties, only the kernel data and its hashes differ, hashes match
    pb = props(parse(out))
    if set(pb) != set(pr):
        raise FitError("property set changed")
    for key, v in pr.items():
        if key in rep:
            continue
        if pb[key] != v:
            raise FitError("property %s/%s changed" % key)
    if gzip.decompress(pb[(KERNEL, "data")]) != kernel:
        raise FitError("kernel mismatch after rebuild")
    for path in hashes:
        if _hash(pb[(path, "algo")], pb[(KERNEL, "data")]) != pb[(path, "value")]:
            raise FitError("hash %s mismatch" % path)
    return out, "kernel %d -> %d bytes (zopfli), FIT %d -> %d bytes" % (len(kz), len(nz), len(data), len(out))
