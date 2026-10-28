#!/usr/bin/env python3
"""RAM-boot an rtos.itb on the Pluto Nano from the Mac: nothing is written to the QSPI flash.

Needs both USB-C ports on the Mac: the CH340 console (UART1) and the Pluto's data port (DFU).
Steps: reboot (or wait for a power cycle), stop the Tezuka U-Boot menu (spaces, then ESC to the
NANO> prompt), `run dfu_ram`, dfu-util download to ${fit_load_address}, Ctrl-C, optional
nexus_pl_init (config-2 loads the bitstream), `bootm ${fit_load_address}#<config>`, then log the
console for a while. Everything the console prints goes to the log file.

usage: rtos/tools/ramboot.py rtos.itb [--config config-1] [--tty /dev/cu.usbserial-XXXX]
                             [--log ramboot.log] [--watch 60]
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import termios
import time

ap = argparse.ArgumentParser()
ap.add_argument("itb")
ap.add_argument("--config", default="config-1")
ap.add_argument("--tty", default=None)
ap.add_argument("--log", default="ramboot.log")
ap.add_argument("--watch", type=int, default=60)
ap.add_argument("--wait-boot", type=int, default=180, help="seconds to wait for U-Boot after asking for a power cycle")
a = ap.parse_args()

tty = a.tty or next(iter(sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.wchusbserial*"))), None)
if not tty:
    sys.exit("no CH340 console port: plug the Pluto console USB-C into the Mac")
if not os.path.isfile(a.itb):
    sys.exit(f"no image {a.itb}")

fd = os.open(tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
at = termios.tcgetattr(fd)
at[0] = 0                                    # iflag
at[1] = 0                                    # oflag
at[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
at[3] = 0                                    # lflag: raw
at[4] = at[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, at)
log = open(a.log, "ab")
buf = bytearray()


def pump(t=0.05):
    end = time.time() + t
    while time.time() < end:
        try:
            d = os.read(fd, 4096)
        except BlockingIOError:
            d = b""
        if d:
            buf.extend(d)
            log.write(d)
            log.flush()
            sys.stdout.write(d.decode("latin-1"))
            sys.stdout.flush()
        else:
            time.sleep(0.01)


def send(s):
    os.write(fd, s.encode() if isinstance(s, str) else s)


def wait_for(pat, timeout, every=None, tick=None):
    """Wait for regex pat in new console output; optionally send `every` each `tick` s."""
    start = len(buf)
    end = time.time() + timeout
    nxt = 0.0
    while time.time() < end:
        if every is not None and time.time() >= nxt:
            send(every)
            nxt = time.time() + tick
        pump(0.05)
        if re.search(pat, bytes(buf[start:]).decode("latin-1")):
            return True
    return False


print(f"== console {tty}, image {a.itb} ({os.path.getsize(a.itb)} B), config {a.config}")
send("\r")
pump(1.0)
tail = bytes(buf[-400:]).decode("latin-1")
if re.search(r"[#$] ?$", tail.strip() + " ") and "NANO>" not in tail:
    print("\n== Linux shell on the console: reboot")
    send("reboot\r")
elif "NANO>" in tail:
    print("\n== already at the U-Boot prompt")
else:
    print("\n== no shell prompt (login prompt or silent): power-cycle the Pluto now")

if "NANO>" not in tail:
    if not wait_for(r"U-Boot 20", a.wait_boot):
        sys.exit(f"\nno U-Boot banner within {a.wait_boot} s")
    # stop autoboot / menu, then leave the menu with ESC to the NANO> prompt
    wait_for(r"NexusBS|autoboot|Hit any key|menu", 15, every=" ", tick=0.1)
    if not wait_for(r"NANO>", 30, every="\x1b", tick=0.3):
        sys.exit("\nno NANO> prompt")

send("\r")
pump(0.5)
print("\n== DFU RAM mode")
send("run dfu_ram\r")
if not wait_for(r"DFU|dfu", 15):
    sys.exit("\nU-Boot did not enter DFU")
time.sleep(1.5)
r = subprocess.run(["dfu-util", "-a", "firmware.dfu", "-D", a.itb], capture_output=True, text=True)
print(r.stdout[-1500:], r.stderr[-800:])
log.write(("\n[dfu-util rc=%d]\n%s\n%s\n" % (r.returncode, r.stdout, r.stderr)).encode())
pump(1.0)
send("\x03")                                 # end the dfu command
if not wait_for(r"NANO>", 10, every="\r", tick=2):
    sys.exit("\nno prompt after DFU")
if r.returncode != 0 or "Download done" not in (r.stdout + r.stderr):
    sys.exit("\ndfu-util download failed")

send("iminfo ${fit_load_address}\r")
wait_for(r"NANO>", 10)
if a.config != "config-1":
    send("run nexus_pl_init\r")
    wait_for(r"NANO>", 5)
print(f"\n== bootm {a.config}")
send("bootm ${fit_load_address}#%s\r" % a.config)
wait_for(r"$^", a.watch)                     # just log for --watch seconds
print(f"\n== log in {a.log}")
