#!/usr/bin/env python3
"""Decode IOB33 pad configuration from a 7-series bitstream with the prjxray
database: which package pins are configured, their IOSTANDARD, direction and
pull. Usage: iob_decode.py <bit> <db-root> <part-dir e.g. xc7z010clg400-1>"""
import sys, json, os, re, csv
sys.path.insert(0, os.path.dirname(__file__))
from frames import parse
bit, dbroot, part = sys.argv[1], sys.argv[2], sys.argv[3]
frames, _ = parse(bit)
tilegrid = json.load(open(f'{dbroot}/zynq7/xc7z010/tilegrid.json'))
pins = {}
for row in csv.DictReader(open(f'{dbroot}/zynq7/{part}/package_pins.csv')):
    pins[row['site']] = (row['pin'], row['bank'], row['tile'], row['pin_function'])
def load_segbits(path):
    feats = {}
    for line in open(path):
        parts = line.split()
        if not parts: continue
        feats[parts[0]] = [(b.lstrip('!'), not b.startswith('!')) for b in parts[1:]]
    return feats
segbits = {t: load_segbits(f'{dbroot}/zynq7/segbits_{t.lower()}.db') for t in ('LIOB33', 'RIOB33')}
# SING tiles use the same tile-type segbits with IOB_Y0 only
def bit_set(base, offset, spec):
    fr, bitidx = spec.split('_'); fr = int(fr); bitidx = int(bitidx)
    frame = frames.get(base + fr)
    if frame is None: return False
    word = offset + bitidx // 32
    if word >= len(frame): return False
    return (frame[word] >> (bitidx % 32)) & 1 == 1
result = {}
for tname, tile in tilegrid.items():
    ttype = tile.get('type', '')
    if not (ttype.startswith('LIOB33') or ttype.startswith('RIOB33')): continue
    bits = tile.get('bits', {}).get('CLB_IO_CLK')
    if not bits: continue
    base = int(bits['baseaddr'], 16); offset = bits['offset']
    family = 'LIOB33' if ttype.startswith('LIOB33') else 'RIOB33'
    feats = segbits[family]
    for site, stype in tile['sites'].items():
        # which IOB_Y? site index: IOB_X?Y(odd) -> Y1 within a 2-site tile; SING tiles only Y0
        y = int(site.split('Y')[1])
        yk = 'IOB_Y0' if (ttype.endswith('SING') or y % 2 == 0) else 'IOB_Y1'
        setf = []
        for fname, spec in feats.items():
            if not fname.startswith(f'{family}.{yk}.'): continue
            ok = all(bit_set(base, offset, s) == want for s, want in spec)
            if ok: setf.append(fname.split('.', 2)[2])
        if setf:
            pin = pins.get(site, ('?', '?', '', ''))
            result[pin[0]] = {'site': site, 'bank': pin[1], 'features': sorted(setf)}
def summarize(f):
    std = next((x for x in f if re.match(r'^(LVCMOS|LVTTL|LVDS|SSTL|HSTL|DIFF)', x) and '.' not in x.split('.')[0][:0]), None)
    stds = sorted({x.split('.')[0] for x in f if re.match(r'^(LVCMOS|LVTTL|LVDS|SSTL|HSTL|DIFF|TMDS)', x)})
    drive = [x for x in f if 'DRIVE' in x]; slew = [x for x in f if 'SLEW' in x]
    pull = [x.split('.')[-1] for x in f if x.startswith('PULLTYPE')]
    in_only = 'IN_ONLY' in f
    direction = 'in' if in_only else ('out/inout' if drive else ('in?' if any(x.endswith('.IN') or '.IN_' in x for x in f) else 'cfg'))
    return ','.join(stds) or '-', direction, ','.join(pull) or '-', ','.join(d.split('.')[-1] for d in drive) or '-'
print(f'# {os.path.basename(os.path.dirname(bit))}/{os.path.basename(bit)}: {len(result)} configured pads')
for pin in sorted(result, key=lambda p: (p[0], int(re.sub(r"\D", "", p) or 0))):
    r = result[pin]; stds, direction, pull, drive = summarize(r['features'])
    print(f'{pin:5s} bank{r["bank"]:>3s} {r["site"]:12s} {stds:10s} {direction:9s} pull={pull:8s} drive={drive}')
json.dump(result, open(bit + '.iob.json', 'w'), indent=1)
