# Flashing the Pluto Nano: step by step

A plain, follow-along guide for flashing the HamGeek Pluto Nano (NanoSDR) firmware over USB
DFU, and for connecting to its serial console. Nothing here needs special tools beyond a Mac or
Linux PC, two USB cables and `dfu-util`.

> **Golden rule:** always **RAM-boot a new image first**. A RAM boot writes nothing to flash; if
> the image is bad, a power cycle brings back the old firmware. Flash only after the RAM boot
> passes your checks.

## 1. What you need

| Item | Notes |
|---|---|
| Pluto Nano | Powered from its USB or an external 5 V supply |
| USB **data** cable for the **main USB port** | Carries DFU (firmware upload). Charge-only cables look identical and do not work |
| USB cable for the **serial (UART) port** | The board's CH340 USB-serial chip (`1a86:7523`). It is powered by the board, so it only appears while the Pluto has power |
| `dfu-util` | See section 1a |
| A serial terminal | macOS/Linux: `screen`, `picocom`, `minicom` or `tio`; Windows: PuTTY |
| Firmware files | `pluto.frm` (for mass-storage update) and `pluto.dfu` (for DFU) from the CI build |

The **main USB port** and the **serial port** are two different connectors. DFU only works on the
main port. If you are not sure which is which, plug in only the serial cable first: the port that
makes a `usbserial` device appear is the serial port; the other one is the main port.

Unplug any USB Wi-Fi dongle from the Pluto while flashing.

## 1a. Install the tools

### macOS

```sh
brew install dfu-util          # Homebrew: https://brew.sh
dfu-util --version
```

`screen` is already installed. The CH340 serial chip works with the driver built into current
macOS (no extra driver needed).

### Linux (Debian, Ubuntu, Raspberry Pi OS)

```sh
sudo apt install dfu-util picocom
```

Fedora: `sudo dnf install dfu-util picocom`; Arch: `sudo pacman -S dfu-util picocom`.

To use the devices without `sudo`, add yourself to the serial group and allow the Pluto's DFU
device, then log out and back in:

```sh
sudo usermod -aG dialout $USER
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0456", ATTR{idProduct}=="b674", MODE="0666"' | \
  sudo tee /etc/udev/rules.d/53-pluto-dfu.rules
sudo udevadm control --reload-rules
```

### Windows 10/11

1. **dfu-util**: download the latest Windows build (`dfu-util-<version>-binaries.tar.xz`) from
   <https://dfu-util.sourceforge.net/releases/>, extract it (7-Zip opens `.tar.xz`), and use
   `dfu-util.exe` from the `win64` folder in a Command Prompt or PowerShell opened in that folder.
2. **USB driver for DFU** (needed once): put the Pluto in DFU mode (section 5 or 6), run
   **Zadig** (<https://zadig.akeo.ie/>), choose *Options > List All Devices*, select the
   "PlutoSDR DFU" device (`0456:b674`) and install the **WinUSB** driver. Without this,
   `dfu-util -l` on Windows finds nothing.
3. **Serial driver**: Windows 10/11 usually installs the CH340 driver automatically. If no COM
   port appears in Device Manager under *Ports (COM & LPT)*, install the CH341SER driver from the
   chip maker WCH (<https://www.wch-ic.com/downloads/CH341SER_EXE.html>).
4. **Serial terminal**: PuTTY (<https://www.putty.org/>): Connection type *Serial*, the COM port
   from Device Manager (for example `COM5`), speed `115200`.

On Windows, use the same `dfu-util` commands as below with `dfu-util.exe`, and compute the size
for `fit_size` in PowerShell with `'{0:X}' -f (Get-Item pluto.dfu).Length`.

## 2. Connect the serial console

Settings: **115200 baud, 8 data bits, no parity, 1 stop bit (8N1), no flow control.**

1. Plug the serial cable into the Pluto and the computer.
2. Find the device:
   - macOS: `ls /dev/cu.usbserial-*` (for example `/dev/cu.usbserial-2110`; the number changes
     with the USB port and hub you use)
   - Linux: `ls /dev/ttyUSB*` (usually `/dev/ttyUSB0`)
   - Windows: Device Manager > *Ports (COM & LPT)* > "USB-SERIAL CH340 (COMx)"
3. Open it:
   ```sh
   screen /dev/cu.usbserial-2110 115200        # macOS (quit: Ctrl-A then K, then y)
   picocom -b 115200 /dev/ttyUSB0              # Linux (quit: Ctrl-A then Ctrl-X)
   ```
4. Press **Enter**. You should see:
   ```
   Welcome to NexusBS 2.0
   pluto login:
   ```
5. Log in as `root` with the device's root password.

Nothing shows up? Check the baud rate, press Enter a few times, and make sure the Pluto is
powered. Garbage characters mean a wrong baud rate.

## 3. Get to the U-Boot prompt

U-Boot is the boot loader. Flashing and RAM boot are done from its `NANO>` prompt.

1. At the Linux shell (logged in), type `reboot`.
2. **Immediately and repeatedly press Space** (keep tapping for up to 30 s) until the boot menu
   "NexusBS Pluto" appears.
3. Press **Esc** to leave the menu. You get the prompt:
   ```
   NANO>
   ```

Note: `device_reboot ram` / `device_reboot sf` from Linux do **not** enter DFU on this board
(its U-Boot boots straight from flash); always use the serial console method above.

## 4. Check that the computer sees the Pluto over USB

Plug the **data cable** into the Pluto's **main USB port**, preferably directly into the computer
(no hub). Then, in step 5, while U-Boot is in DFU mode, run:

```sh
dfu-util -l
```

You must see lines starting with `Found DFU: [0456:b674]`. If you see nothing:

- the cable is charge-only (most common), or
- the cable is in the serial port instead of the main port, or
- a hub/adapter is in the way.

On macOS you can also check `system_profiler SPUSBDataType`: a working setup shows both the
"USB Serial" device and the Pluto.

## 4a. Back up before you flash

Do this once before the first flash, and keep the files somewhere safe. They contain your Wi-Fi
password and other private settings: **never publish or commit them**.

### Your settings and applications (`/mnt/data`)

`/mnt/data` holds the configuration, the applications and the Bluetooth pairings. Flashing
normally keeps it, but back it up anyway. With the Pluto running and on the network:

```sh
ssh root@<pluto-ip> 'tar -C /mnt -cf - data' > pluto-mnt-data.tar
ssh root@<pluto-ip> 'fw_printenv' > pluto-uboot-env.txt
```

On Windows use the same commands in PowerShell (OpenSSH is built into Windows 10/11).

To restore `/mnt/data` later:

```sh
ssh root@<pluto-ip> 'tar -C /mnt -xf -' < pluto-mnt-data.tar
```

### The flash itself (full image backup over DFU)

This copies the raw flash areas, so you can always go back to exactly what was there. Get to
`NANO>` (section 3), run `run dfu_sf`, then on the computer:

```sh
dfu-util -l                                            # shows the areas listed below
dfu-util -a boot.dfu            -U backup-boot.dfu
dfu-util -a uboot-env.dfu       -U backup-uboot-env.dfu
dfu-util -a uboot-extra-env.dfu -U backup-uboot-extra-env.dfu
dfu-util -a spare.dfu           -U backup-spare.dfu
dfu-util -a firmware.dfu        -U backup-firmware.dfu # 30 MiB area, takes a few minutes
```

`-U` (upload) only **reads** from the Pluto; it never writes. Press Ctrl-C on the console when
done. If a file already exists, dfu-util refuses to overwrite it: delete it or use a new name.

To restore the firmware area later: `run dfu_sf`, then
`dfu-util -a firmware.dfu -D backup-firmware.dfu` and set `fit_size` as in section 6 (use the
size of the *original* image, not of the 30 MiB backup: read it from `pluto-uboot-env.txt`).
Restore `boot.dfu` or the environment areas only if they were damaged; a mistake there can
leave the board without a boot loader (JTAG recovery).

## 5. RAM boot (safe test, writes nothing)

At the `NANO>` prompt:

```
NANO> run dfu_ram
Entering DFU RAM mode ...
```

On the computer:

```sh
dfu-util -a firmware.dfu -D pluto.dfu -R
```

Wait for `Download done`. Back on the serial console, press Enter, then paste this single line:

```
run adi_loadvals; run nexus_pl_init; setenv bootargs console=ttyPS0,115200 maxcpus=${maxcpus} rootfstype=ramfs root=/dev/ram0 rw clk_ignore_unused; bootm ${fit_load_address}#${fit_config}
```

Linux boots from RAM. Log in and test everything (see section 8). To go back to the old
firmware, just power-cycle the board.

## 6. Flash (writes the image permanently)

Only after the RAM boot passed.

1. Get to `NANO>` again (section 3).
2. Start flash DFU:
   ```
   NANO> run dfu_sf
   Entering DFU SF mode ...
   ```
3. On the computer:
   ```sh
   dfu-util -a firmware.dfu -D pluto.dfu
   ```
   Wait for `Download done` and for the flash write to finish on the console. **Do not unplug
   power during this step.**
4. Press **Ctrl-C** on the console to leave DFU and get back to `NANO>`.
5. **Set the image size by hand** (on this board `dfu_sf` does not always store it). The value is
   the size of `pluto.dfu` in hexadecimal:
   ```sh
   printf '%X\n' $(stat -f %z pluto.dfu)      # macOS
   printf '%X\n' $(stat -c %s pluto.dfu)      # Linux
   ```
   Then on the console (example value):
   ```
   NANO> setenv fit_size CF9837
   NANO> saveenv
   ```
   A wrong `fit_size` means U-Boot reads a truncated image and the board does not boot.

## 7. Verify before rebooting

U-Boot has no md5, so compare a CRC32:

```
NANO> sf probe 0:0 50000000 0
NANO> sf read ${fit_load_address} 0x200000 ${fit_size}
NANO> crc32 ${fit_load_address} ${fit_size}
NANO> iminfo ${fit_load_address}
```

Compare the CRC with the file on the computer:

```sh
python3 -c "import sys,zlib;print('%08x'%zlib.crc32(open(sys.argv[1],'rb').read()))" pluto.dfu
```

The two values must match and `iminfo` must say the hashes are OK. Then:

```
NANO> reset
```

## 8. After boot: quick checks

- `cat /opt/VERSIONS` shows the expected version.
- `dmesg | grep -i -E 'error|fail|taint'` shows nothing unexpected.
- Wi-Fi gets an address (`ip addr show wlan0`), DNS works (`nslookup pool.ntp.org`).
- `/mnt/data` is mounted and your configuration is still there (`ls /mnt/data`).
- From the computer: `ssh root@<pluto-ip>` and `scp somefile root@<pluto-ip>:/tmp/` work.

## 9. If something goes wrong

| Symptom | What to do |
|---|---|
| No `usbserial` device | Pluto not powered, or bad serial cable/port |
| Console silent or garbage | Wrong baud rate; must be 115200 8N1 |
| Boot menu never appears | Start tapping Space right after `reboot`, keep going up to 30 s |
| `dfu-util -l` finds nothing | Charge-only cable, wrong port, or hub. See section 4 |
| `LIBUSB_ERROR_OTHER` during upload | U-Boot may have left DFU; run `dfu_ram`/`dfu_sf` again and retry |
| RAM-booted image misbehaves | Power-cycle: the flash still holds the old firmware |
| Flashed image does not boot | Get to `NANO>` (U-Boot lives in a separate flash area and is untouched), check `fit_size`, flash the previous known-good `pluto.dfu` again |
| No U-Boot at all | JTAG recovery (see the repository README) |

Never flash an image much bigger than the last one that is known to boot: the image partition
and `fit_size` have a hard limit (14 MiB on this board's layout).

## Appendix: the alternative mass-storage update

A running stock Pluto firmware also exposes a USB drive on the main port: copy `pluto.frm`
to it, eject it, and wait until the LED stops blinking. It is convenient but gives no RAM-boot
safety net, so prefer the DFU procedure above for new or untested images.
