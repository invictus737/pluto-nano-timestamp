# tools/pluto: observe, deploy and A/B the Pluto Nano RTEMS station

These tools follow the Nexus-Light approach: the board serves its own numbers, a poller on
the Mac reads them, an OTA update takes about a minute, and every change is judged with
measured numbers against a budget. Python 3 standard library only. The `ssh` client, `gh`
(only for CI release tags) and `zopfli` (optional, for repacking) are external commands.

| tool | what it does |
|---|---|
| `pluto-watch` | live terminal view of `/metrics` (1-2 s), red flags on budget violations, JSONL recorder, log follower |
| `pluto-deploy` | `.itb` or CI release tag, zopfli repack, `fwupdate` over TCP, reboot, wait for `/metrics`, print the build id |
| `pluto-ab` | scenario A then B (env switches, layouts, builds, or manual) for N minutes each, comparison table, PASS/FAIL |
| `pluto-modes` | per-mode cost matrix (TETRA, +DMR, +P25, +FM) and an isolated, idempotent per-mode on/off check |

## What the board needs

1. **An image built from pluto-rtos commit `1448c76` or later** (`rtos/app/http_status.c`,
   `metrics.c`). Use `rtos-nexus2-cherry.itb` (the station with CherryUSB) for the full set.
   The base image (`rtos.itb`) serves `/metrics` without the station sections (`stages`,
   `ad9363` and `nexus2.state` are absent); libbsd USB images report `"usb": null`.
2. **Port 80 reachable** from the Mac (Wi-Fi or USB Ethernet). Nothing else is needed for
   `pluto-watch`.
3. **For commands** (`pluto-ab` env and layout scenarios, `nexus stages reset`), use one of:
   - a token: on the board, `config set metrics_token <16+ random chars>`. The value is
     masked like every `*token*` key. On the Mac, run `export PLUTO_TOKEN=...`. Without the
     key `/cmd` answers 403 and runs nothing.
   - SSH exec: `--ssh-key ~/.ssh/<key> --known-hosts <file>`. The key must be one of the
     image's authorized keys (`rtos/ssh/authorized_keys`, or `ssh_authorized_key` in the
     config), and the board's host key must be in the known_hosts file. Host keys are
     checked strictly.
4. **For `pluto-deploy`** SSH is required, because `fwupdate` is started through SSH exec.
5. **For `pluto-modes`** the nexus2 dashboard must be reachable (default
   `http://<board-ip>:8080`). Without `[dashboard.auth]` nexus2 only answers requests whose
   Host is its own `ip:port`, so address the board by IP. With auth, pass `--dash-user` and
   `--dash-pass-env VAR`.

Environment defaults: `PLUTO_HOST`, `PLUTO_TOKEN`, `PLUTO_SSH_KEY`, `PLUTO_KNOWN_HOSTS`.

## Board endpoints (port 80, `rtos/app/http_status.c`)

All of them run in one low-priority thread on CPU0 and compute only what is requested. No
periodic work is added and the radio threads are never stopped.

- `GET /`: the plain-text status page (as before).
- `GET /metrics[?top=N&full=1&stacks=1&events=1&hw=0]`: JSON, schema `nexus.metrics/1`:
  - `uptime_s`, `unix_s`, `collect_ms`
  - `build`: git (12 chars, `-dirty`), CI run, `nexus_ref`, build time, RTEMS version, USB
    stack, and whether this is a station or base image
  - `cpu`: busy % per core over the interval since the previous `/metrics` request (the first
    request reports since boot), plus `busy_pct_boot` and the thread count
  - `threads`: the top N by CPU (name, id, cpu, affinity, prio, %), plus `threads_rest_pct`
  - `nexus2`: linked, state (running/stopped/absent), layout
  - `stages`: nexus2 `nexus stages` rows (count, mean/p99/max us, budget, over, us/s; the
    `drive.turn`, `rx.analysis`, `tx.synthesis`, `tx.write`, `tetra.tick`, `tetra.phy`,
    `drive.wake`, `tx.lead` and `carrierN.<mode>` stages), cumulative since start or the last
    `nexus stages reset`, plus `tx_late`
  - `ad9363`: open, rate, RX/TX LO, AGC mode, RX gain, ENSM (fdd/not_fdd), TX attenuation,
    TX overflow bits (REG 0x05E), discarded TX blocks, temperature. These are a few SPI reads
    under the driver lock, which `hw=0` skips. The driver keeps no RX/TX dBFS levels; the
    dashboard has per-call RSSI.
  - `usb` (CherryUSB): pipes, submits/completions/errors, stalls, kills, handshake misses,
    controller and board-level resets, HSE, wedges, stuck cancels/pending, DMA halts, async
    kicks
  - `net`: interfaces (up, running, link, MTU, packet and error counters, IPv4), default
    route, DNS, Wi-Fi (present, connected, SSID, RSSI, channel, attempts, last error), USB
    Ethernet summary, and with `full=1` the routing-socket events
  - `heap`: size, free, min_free, largest_free, block counts, failed_allocs. These come from
    the heap statistics plus a walk of the free list only.
  - `stacks` (`full=1` or `stacks=1` only): high-water marks, fullest first. This scans every
    stack's unused area through the shared L2, so ask rarely (`pluto-watch` does it every 30 s).
  - `temp.zynq_c`: Zynq XADC through the PS interface, with a bounded poll and `null` on
    failure
  - `log`: the log ring's `next` and `oldest` offsets
- `GET /log?since=N[&max=B]`: `{since, start, next, oldest, gap, text}` from the console log
  ring (256 KiB, station images). Pass `next` back as `since`. `gap` means bytes were lost.
- `GET|POST /cmd?c=...` (`Authorization: Bearer <metrics_token>`): runs one command from the
  allowlist and returns `{argv, rc, ms, output}`. The allowlist:
  - `nexus start|stop|status`
  - `nexus layout [light|cpu1|float]`
  - `nexus env [pluto|save|clear|NEXUS2_*=V|RUST_LOG=V ...]`
  - `nexus stages [reset]`
  - `nexus load [1..10]`
  - `usbh`, `lsusb [-t]`, `net status`, `wifi status`

  Words may only contain `[A-Za-z0-9_=.:,/-]`. Plain HTTP is visible on the LAN, so treat
  the token as a LAN-only convenience and use SSH where that matters.

## pluto-watch

```
tools/pluto/pluto-watch 192.168.1.160                         # refresh every 2 s
tools/pluto/pluto-watch 192.168.1.160 -i 1 --record run.jsonl --log-file run.log
tools/pluto/pluto-watch 192.168.1.160 --once                  # one screen; exit 1 on a FAIL flag, 2 unreachable
```

The screen shows the build, uptime, nexus2 state and layout, CPU0/CPU1, the top threads, the
stage table with a window mean since the previous poll, tx.late, AD9363, USB, network and
Wi-Fi, heap, temperature and the fullest stacks.

Red flags come from `metrics.Budget`:
- deadline core (`--deadline-cpu 1`) above `--max-deadline 60` %
- other core above `--max-other 60` % busy (less than 40 % idle)
- tx.late or a USB error, handshake-miss or reset counter moving
- Wi-Fi disconnected (`--no-wifi-check` for USB Ethernet only)
- a reboot, or nexus2 stopping

Yellow flags are stage over-budget counts, `ad9363.tx_discarded`, USB stalls and XACTERR
restarts.

## pluto-deploy

```
tools/pluto/pluto-deploy 192.168.1.160 rtos-ci-412 --ssh-key ~/.ssh/pluto --known-hosts ~/.ssh/pluto_kh
tools/pluto/pluto-deploy 192.168.1.160 build/rtos-nexus2-cherry.itb --reboot slcr --yes
tools/pluto/pluto-deploy - rtos-ci-412 --dry-run --out /tmp/r.itb      # download + repack only
```

The tool runs these steps and times each one:

1. Get the image: a local file, or a draft release `rtos-ci-<run>` (asset
   `rtos-nexus2-cherry.itb`, change it with `--asset`). For a tag, the release's commit
   becomes the expected build id.
2. Repack with zopfli (the `zopfli` pip module or the `zopfli` command). Every FIT property
   except the kernel data and its hashes stays byte-identical, and the result is verified.
   Without zopfli the image is sent as built.
3. Start `ssh root@board fwupdate 4444 reboot`, send the image to TCP 4444 and wait for
   `fwupdate: OK`. The board skips sectors whose content is unchanged.
4. Reboot. By default fwupdate's `reboot` does it. `--reboot slcr` runs
   `mw32 0xF8000008 0xDF0D` then `mw32 0xF8000200 1`. `--reboot none` does not reboot.
5. Wait for `/metrics` from the new boot and print `build.git`. With a tag, a mismatch exits
   with 3.

The tool asks before flashing unless you pass `--yes`, because each flash or reboot of a live
station needs the operator's OK.

**Serial fallback** (manual, never automatic): if the board does not come back, connect the
console (`/dev/cu.usbserial-*`, 115200 8N1; the Mac CH34x RX is unreliable faster than that).
Stop U-Boot, `loady` the `.itb` over YMODEM, `sf probe; sf update <loadaddr> 0x200000 <size>`,
then `setenv fit_size <size>; saveenv` and `boot`. The bring-up's `serial_flash.py` (RTOS
scratch tooling, see the rtos-optimizer skill `references/rtems-zynq.md`) automates exactly
this. To recover a stuck `loady`, send ESC.

## pluto-ab

```
export PLUTO_TOKEN=...
tools/pluto/pluto-ab 192.168.1.160 --minutes 10 --warmup 60 --rounds 2 \
    --a-label q15-off --a-env NEXUS2_PLUTO_ANALYSIS_Q15=0 \
    --b-label q15-on  --b-env NEXUS2_PLUTO_ANALYSIS_Q15=1 --out ab.json --record ab.jsonl
tools/pluto/pluto-ab 192.168.1.160 --a-layout light --b-layout cpu1 --minutes 5
tools/pluto/pluto-ab 192.168.1.160 --a-image rtos-ci-410 --b-image rtos-ci-412 --ssh-key k --known-hosts kh
tools/pluto/pluto-ab 192.168.1.160 --manual --a-label before --b-label after
```

Each run follows the same sequence:
1. Apply the scenario. Env and layout scenarios run `nexus stop`, `nexus layout`,
   `nexus env` and `nexus start`, and wait for nexus2 to run. Image scenarios run
   `pluto-deploy`. `--a-cmd` and `--b-cmd` run any allowlisted command.
2. Warm up.
3. Run `nexus stages reset`.
4. Poll `/metrics` for `--minutes`.

The table compares:
- CPU0 and CPU1 busy (mean, p99, max)
- each stage's mean, p99 and max µs, its over-budget count and µs/s
- deltas of tx.late, the USB counters and other counters
- Wi-Fi-down polls, minimum free heap and Zynq temperature

A run passes when all of these hold:
- deadline-core mean ≤ `--max-deadline`
- other-core mean ≤ `--max-other`
- tx.late +0
- no USB error, handshake or reset counter moved
- Wi-Fi stayed up, with no reboot and no nexus2 stop
- fewer than 10 % of polls failed

Use `--rounds 2` (A,B,A,B) to repeat runs: one lucky run proves nothing. Env switches persist
for later starts, so B must set every switch A changed (`NAME=` unsets one).

## pluto-modes

```
tools/pluto/pluto-modes 192.168.1.160 --window 60 --cycles 3 --hold 15 --out modes.json
tools/pluto/pluto-modes 192.168.1.160 --dash http://192.168.1.160:8080 --dash-user admin --dash-pass-env NEXUS_DASH_PASS
```

The tool uses `GET /api/status` and `POST /api/control/native`
(`{"channel","enabled","expected_revision"}` with `X-Nexus2-Control: native`, a same-origin
`Origin` and a JSON body). It applies a change only when `desired_enabled` matches, the stage
is `idle` and the worker phase is `bound` (on) or not bound (off).

1. **Cost matrix**: TETRA only, then +DMR, +P25 and +FM, with the others of these four
   disabled. Each step reports CPU0/CPU1, the CPU1 delta, the window means of `drive.turn`,
   `rx.analysis`, `tx.synthesis` and `tetra.tick`, each carrier's µs/s, and tx.late.
2. **Isolation and idempotency**: with all four enabled, each mode (`--include-tetra` adds
   TETRA) goes off and on `--cycles` times. Every other bound channel is sampled each
   `--sample` s, and these must hold:
   - its `service_turns` keeps increasing (TETRA's is its slot counter)
   - its worker phase stays `bound`
   - its intent revision and desired state stay unchanged
   - tx.late does not move

   Every apply is sent a second time with the same intent and must come back `200 unchanged`
   with the revision untouched. The output gives apply latencies and a PASS/FAIL per mode.

The initial desired states are restored at the end (`--no-restore` keeps the last state).

## Tests

```
python3 -m unittest discover -s tools/pluto/tests -v
```

- `test_board_fmt.py` builds `rtos/app/metrics_fmt.c` with the host `cc` (ASan/UBSan when
  available). It covers the JSON writer, the nexus2 stage-table parser, log escaping
  (control bytes and invalid UTF-8) and the `/cmd` allowlist.
- `test_lib.py` covers flattening, budget flags, statistics and the FIT rebuild/repack with
  md5 and crc32 hashes.
- `test_tools.py` runs all four tools end to end against `fakeboard.py`, a fake status server
  plus a fake nexus2 dashboard with Host/Origin/marker checks, revisions and fault knobs. The
  deploy tests use stand-in `ssh` and `zopfli` commands on `PATH`.

The RTEMS side is not built on the Mac. The C files were syntax-checked against the RTEMS 6.2,
libbsd and newlib headers, and the real build is the CI `rtos` workflow.
