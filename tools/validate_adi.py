#!/usr/bin/env python3
"""Fail unless the decoded pads equal the PL pins (banks 34/35) of ADI's pluto XDC."""
import sys, json, re, csv
iob, xdc, pins = sys.argv[1:4]
pl = {r['pin'] for r in csv.DictReader(open(pins)) if r['bank'] in ('34', '35')}
decoded = {p for p in json.load(open(iob)) if p != '?'}
expected = set(re.findall(r'PACKAGE_PIN\s+([A-Z]+[0-9]+)', open(xdc).read())) & pl
print('decoded', len(decoded), 'expected', len(expected), 'only decoded', sorted(decoded - expected), 'only xdc', sorted(expected - decoded))
sys.exit(0 if decoded == expected else 1)
