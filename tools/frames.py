#!/usr/bin/env python3
"""Reconstruct the configuration frame map of a (possibly compressed) Xilinx
7-series .bit file: {FAR: [101 words]}. Handles FDRI writes with FAR
auto-increment and MFWR replication. No external dependencies."""
import struct, sys, json
FRAME_WORDS = 101
def far_next(far):
    # 7-series FAR: [25:23] block type, [22] top/bottom, [21:17] row, [16:7] column, [6:0] minor.
    # Auto-increment across minor/column is design-specific; we only need the
    # explicit FARs written before each FDRI/MFWR, plus sequential frames
    # within one FDRI burst, so we increment minor and let the caller fix
    # overflow using the tilegrid later if needed.
    return far + 1
def parse(path):
    b = open(path, 'rb').read()
    sync = b.find(bytes.fromhex('AA995566')); assert sync >= 0
    j = sync + 4; n = len(b)
    far = None; frames = {}; last_frame = None; writes = []
    stats = {'fdri_words': 0, 'mfwr': 0, 'far_writes': 0}
    pending_fdri = False
    while j + 4 <= n:
        w = struct.unpack('>I', b[j:j+4])[0]; j += 4
        t = w >> 29
        if t == 1:
            op = (w >> 27) & 3; reg = (w >> 13) & 0x1f; wc = w & 0x7ff
            payload = b[j:j+wc*4]; j += wc*4
            if op != 2: continue
            if reg == 1 and wc == 1:
                far = struct.unpack('>I', payload)[0]; stats['far_writes'] += 1
            elif reg == 2:
                pending_fdri = True
                if wc:
                    writes.append((far, payload))
            elif reg == 10:
                stats['mfwr'] += 1
                if last_frame is not None and far is not None:
                    frames[far] = last_frame
        elif t == 2:
            wc = w & 0x7ffffff
            payload = b[j:j+wc*4]; j += wc*4
            if pending_fdri:
                writes.append((far, payload))
            pending_fdri = False
        else:
            pass
    for far0, payload in writes:
        words = list(struct.iter_unpack('>I', payload)); words = [x[0] for x in words]
        stats['fdri_words'] += len(words)
        nfr = len(words) // FRAME_WORDS
        f = far0
        for k in range(nfr):
            fr = words[k*FRAME_WORDS:(k+1)*FRAME_WORDS]
            frames[f] = fr; last_frame = fr; f = far_next(f)
    return frames, stats
if __name__ == '__main__':
    frames, stats = parse(sys.argv[1])
    print(json.dumps({'frames': len(frames), **stats}))
    fars = sorted(frames)
    print('first FARs:', [hex(x) for x in fars[:5]], 'last:', [hex(x) for x in fars[-3:]])
    nonzero = sum(1 for f in frames.values() if any(f))
    print('non-zero frames:', nonzero)
