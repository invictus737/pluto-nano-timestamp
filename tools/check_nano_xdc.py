#!/usr/bin/env python3
"""Consistency check between the Nano constraint file and the decoded vendor
pads: every vendor-used PL pad must be constrained, and the port direction in
the ADI pluto design must match the pad's configured direction."""
import csv, re, sys, os
root = os.path.join(os.path.dirname(__file__), '..')
xdc = open(os.path.join(root, 'board/nano/system_constr.xdc')).read()
p2port = {m.group(1): m.group(2) for m in re.finditer(r'PACKAGE_PIN\s+([A-Z]+[0-9]+).*?get_ports\s+\{?\s*([^\s\}\]]+(?:\[[0-9]+\])?)', xdc)}
inputs = ('rx_clk_in', 'rx_frame_in', 'rx_data_in', 'spi_miso', 'clk_out')
outputs = ('tx_clk_out', 'tx_frame_out', 'tx_data_out', 'enable', 'txnrx', 'spi_csn', 'spi_clk', 'spi_mosi')
bad = 0
for r in csv.DictReader(open(os.path.join(root, 'data/vendor-nano-used-pins.csv'))):
    port = p2port.get(r['pin'])
    if port is None:
        print('UNCONSTRAINED vendor pad', r['pin'], r['direction']); bad += 1; continue
    base = re.sub(r'\[\d+\]', '', port)
    if base in inputs and r['direction'] != 'in' or base in outputs and r['direction'] != 'out':
        print('DIRECTION MISMATCH', r['pin'], port, 'vendor', r['direction']); bad += 1
ports = {re.sub(r'\[\d+\]', '', p) for p in p2port.values()}
for required in ('rx_clk_in', 'rx_frame_in', 'rx_data_in', 'tx_clk_out', 'tx_frame_out', 'tx_data_out', 'enable', 'txnrx', 'gpio_resetb', 'gpio_en_agc', 'gpio_ctl', 'gpio_status', 'spi_csn', 'spi_clk', 'spi_mosi', 'spi_miso', 'clk_out', 'pl_gpio'):
    if required not in ports: print('MISSING port', required); bad += 1
print(len(p2port), 'constrained pins;', 'OK' if not bad else f'{bad} problems')
sys.exit(1 if bad else 0)
