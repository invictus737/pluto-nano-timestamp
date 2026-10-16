#!/usr/bin/env python3
"""Classify decoded IOB pads (from iob_decode.py *.iob.json) into used inputs
and outputs, and compare pin sets between designs and XDC files.
usage: compare_pins.py <reference.iob.json> [other.iob.json|other.xdc ...]"""
import sys, json, re
def classify(features):
    f = set(features)
    if any('DRIVE' in x for x in f): return 'out'
    if any(x == 'IN_ONLY' or x.endswith('.IN') or '.IN_' in x for x in f): return 'in'
    return 'unused'
def used(path):
    if path.endswith('.xdc'):
        return {p: '?' for p in re.findall(r'PACKAGE_PIN\s+([A-Z]+[0-9]+)', open(path).read())}
    return {p: classify(v['features']) for p, v in json.load(open(path)).items() if p != '?' and classify(v['features']) != 'unused'}
def key(p): return (p[0], int(p[1:]))
if __name__ == '__main__':
    ref = used(sys.argv[1]); R = set(ref)
    print(f'{sys.argv[1]}: {len(R)} used pads, {sum(1 for d in ref.values() if d == "in")} in, {sum(1 for d in ref.values() if d == "out")} out')
    for p in sorted(R, key=key): print(f'  {p:4s} {ref[p]}')
    for other in sys.argv[2:]:
        o = used(other); O = set(o)
        same = sum(1 for p in R & O if ref[p] == o[p] or o[p] == '?')
        print(f'{other}: used {len(O)}, common {len(R & O)} (same direction {same}), only reference {sorted(R - O, key=key)}, only other {sorted(O - R, key=key)}')
