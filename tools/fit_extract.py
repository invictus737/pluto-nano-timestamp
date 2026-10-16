#!/usr/bin/env python3
"""Extract one image's data from a FIT (flattened device tree) firmware file.
usage: fit_extract.py <fit> <image-name e.g. fpga@1> <out>"""
import struct, sys
def parse(b):
    magic, totalsize, off_struct, off_strings, off_mem, version, lastcomp, boot_cpu, size_strings, size_struct = struct.unpack('>10I', b[:40])
    assert magic == 0xd00dfeed
    strings = b[off_strings:off_strings+size_strings]
    def name_at(o): e = strings.index(b'\0', o); return strings[o:e].decode()
    pos = off_struct; path = []; tree = {}
    while True:
        tok = struct.unpack('>I', b[pos:pos+4])[0]; pos += 4
        if tok == 1:
            e = b.index(b'\0', pos); name = b[pos:e].decode(); pos = (e + 1 + 3) & ~3
            path.append(name); tree.setdefault('/'.join(path), {})
        elif tok == 2: path.pop()
        elif tok == 3:
            ln, noff = struct.unpack('>II', b[pos:pos+8]); pos += 8
            tree['/'.join(path)][name_at(noff)] = b[pos:pos+ln]; pos = (pos + ln + 3) & ~3
        elif tok == 4: pass
        elif tok == 9: break
    return tree
if __name__ == '__main__':
    fit, name, out = sys.argv[1:4]
    tree = parse(open(fit, 'rb').read())
    node = next(k for k in tree if k.endswith('/' + name) or k == name)
    open(out, 'wb').write(tree[node]['data']); print(out, len(tree[node]['data']), 'bytes')
