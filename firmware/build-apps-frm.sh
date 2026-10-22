#!/usr/bin/env bash
# Assemble apps.frm (application slot, see README "Application slot") from prepared inputs. Linux/CI.
#   apps.frm = 4096-byte header | payload (ustar tar, xz --check=crc32) | [boot.bin] | md5 trailer (33 B)
# usage: build-apps-frm.sh <inputs dir> <output dir>
# Outputs (same payload): apps.frm (no boot.bin: the installed FSBL/clock stays) and
#   apps-stock-clock.frm (+ the Tezuka standard-clock FSBL, a recovery path back from an overclock; the
#   normal way to change the clock is config.txt cpu_clock / nexus-clock)
