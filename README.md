# DiskIOStress

A Linux block-device **data-integrity** test tool for NVMe / SATA / USB storage.
It writes data carrying per-sector integrity tags, reads it back, and verifies it
— to catch firmware / controller / media data-corruption bugs.

It has grown two distinct capabilities:

| Mode | Invocation | Use it when… |
|------|------------|--------------|
| **Stress test** | `DiskIOStress [opts] <dev>` | continuous write/read/compare hammering of a device. Single-thread io_uring + `O_DIRECT`, high queue depth — works on NVMe/SATA/USB. |
| **Single command** | `DiskIOStress <dev> <subcmd> …` | a one-off op: read/write/trim/throughput-profile (io_uring, any transport) or NVMe admin (SMART log, firmware, controller reset). |
| **Power-cycle test** | `DiskIOStress <dev> pcwrite …` | verify data integrity across **repeated power loss** (graceful + ungraceful), incl. PLP validation. Single-thread io_uring + `O_DIRECT`, works on NVMe/SATA/USB. |

All three share the same per-sector tag + verification scheme (see
[Sector tags](#sector-tags--integrity-verification)).

> ⚠️ **This tool writes directly to the raw block device and destroys all data on
> the target.** Before any destructive run it prints a clean device list (disks +
> model + mountpoints, no loop devices) and asks to confirm. If the target carries
> a **system mount** (`/` or `/boot`) it refuses the easy path and makes you type
> the full device path — so you can't accidentally wipe your system drive. For CI,
> `--yes` skips the confirmation (but a system drive is still refused, not wiped).

---

## Prerequisites

* Linux kernel ≥ 5.1 (all data IO now goes through io_uring)
* `liburing-dev` (build-time) — `sudo apt install liburing-dev`
* GCC ≥ 4.0, GNU make

## Build & test

```
make            # -> build/DiskIOStress
make test       # build + run the Unity unit-test suite
make clean      # remove build/ and generated logs
```

---

## Mode 1 — Stress test

Single-thread, high-queue-depth io_uring (block-layer + `O_DIRECT`) write →
read → verify over the device, looping until it completes, errors, or hits the
time limit. Works on NVMe / SATA / USB.

```
sudo ./build/DiskIOStress /dev/sdb
sudo ./build/DiskIOStress --qd=64 --io-size=128K --pattern=addr --ranges=0-4G /dev/nvme0n1
# run for a fixed time instead of counting loops:
sudo ./build/DiskIOStress --test-time=10m --io-size=64K /dev/sdb
```

While running (on a terminal) it shows a **live dashboard** that updates in place:
line 1 = elapsed + bytes written/read + loop/gen; then write and read throughput,
each as a **2-row** sparkline (16 levels of resolution) so a performance dip
stands out:

```
[  42s] written 12.5 GB  read 11.8 GB  loop 6  gen 6
  write    512.7 MB/s  ▇▇█▆▇▅▅
                       ███████
  read     430.1 MB/s   ▆▆▇█▇
                       ▃█████
```

The multi-line dashboard needs a terminal (ANSI). When stdout is piped/redirected
it falls back automatically; or pass **`--simple-progress`** to force a single
line updated every 5 s with the interval average — works in any environment
(CI logs, `screen`, dumb terminals):

```
[  600s] write   240.5 MB/s  read   370.2 MB/s  (written 11.0 GB, read 10.8 GB)
```

The startup banner reports the device's **link speed** (e.g. `PCIe Gen4 x4` for
NVMe, `USB3 (5 Gbps)` for USB), and at the end a **Test Report** prints the
result (PASS/FAIL), duration, bytes written/read and average throughput:

```
========== Test Report ==========
  result    : PASS
  device    : /dev/sdb  (USB3 (5 Gbps) [USB 3.20])
  workload  : rand_wrc   pattern: random   seed: 0x6A0D6480
  duration  : 600 s
  passes    : 88 (generation)
  written   : 11.0 GB   (avg 240.5 MB/s)
  read      : 10.8 GB   (avg 370.2 MB/s)
  errors    : 0
=================================
```

Settings come from **CLI > config file > built-in defaults**:

| Option               | Meaning                              |
|----------------------|--------------------------------------|
| `-c, --config=PATH`  | load an INI config file (`DiskIOStress.conf.example`) |
| `-q, --qd=N`         | io_uring queue depth (default 32)    |
| `-I, --io-size=N`    | IO unit size, bytes/K/M (default 64K) |
| `--ranges=`          | `whole` (default), or `0-1G,4G-5G`   |
| `--align=N`          | alignment boundary to test (e.g. `4K`) |
| `--align-mode=`      | `aligned` \| `unaligned` \| `mixed` (with `--align`) |
| `--align-offset=`    | unaligned offset in bytes, or `random` (default 1 sector) |
| `--trim`             | TRIM the range after each verify pass |
| `--verify-only`      | read-only scrub: verify magic/LBA/CRC, no writes (no confirm) |
| `--no-direct`        | disable `O_DIRECT` (e.g. on a file)  |
| `-l, --loops=N`      | max passes (default huge — usually let `--test-time` bound it) |
| `-D, --test-time=N`  | run for this long: `30s` / `10m` / `2h` / `1d` (checked promptly mid-pass) |
| `-p, --pattern=NAME` | data pattern (see below)             |
| `-w, --workload=NAME`| workload (see below)                 |
| `-s, --seed=N`       | RNG seed (0 = derive from time)      |
| `-y, --yes`          | skip the erase confirmation (CI/non-interactive); still refuses system drives |
| `--simple-progress`  | single-line 5 s-average progress instead of the multi-line dashboard |
| `--read-retries=N`   | on a verify mismatch, re-read the sector up to N times (default 3) to classify *transient* (wrong-then-right = FW bug) vs *persistent* — **both fail** |
| `--continue-on-error`| keep running and tally errors after a mismatch (default: stop at the first one) |

> `--threads` / `--trunk-size` from the old multi-thread design are still
> accepted but ignored; concurrency now comes from io_uring queue depth (`--qd`).

**Workloads** (each loop = one generation; the tag's generation counter detects
stale data): `seq_wrc` (write→verify, sequential), `seq_wrrc` (write→verify
twice), `seq_w1rcn` (write once on loop 0, re-verify every later loop —
retention), `rand_wrc` (random full-coverage order, default).

> **Verify is always on** in stress mode — every workload writes then reads back
> and compares the per-sector tags. To verify *previously written* data without
> rewriting (e.g. a scrub the next day), use `--verify-only`: it opens the device
> read-only and checks magic / LBA / CRC of every sector (no generation/seed
> needed, no confirmation prompt).

```
# scrub a region read-only (must use the same --io-size/--ranges you wrote with)
sudo ./build/DiskIOStress --verify-only --io-size=64K --ranges=0-128M /dev/sdb
```

**On a mismatch**, the engine re-reads that sector up to `--read-retries` times
(default 3) to classify the fault — but **either class fails the run**:

- **persistent** (`PERSISTENT ERROR …` + a sector dump) — every re-read still
  mismatches: the data on the media is genuinely wrong (bit-rot, torn/lost
  write, misdirection).
- **transient** (`TRANSIENT READ ERROR …`) — the first read was wrong but a
  re-read returned the correct bytes. For a sector that *was* written
  correctly, a wrong-then-right read means the **drive's firmware/controller
  returned bad data** — a defect, not a tolerable glitch. So it is **not**
  excused; it counts as a failure too.

By default the run **stops at the first error of either kind** (exit non-zero).
Pass `--continue-on-error` to keep going and tally everything — the Test Report's
`errors` line shows both counts (`N persistent, M transient`).

**Patterns** (the bytes written before each compare):

| Pattern | Content | Good for |
|---------|---------|----------|
| `zero` / `one` | `0x00` / `0xFF` | stuck-at cells |
| `working_one` / `working_zero` | walking bit `1<<i` / `~(1<<i)` | line coupling / shorts |
| `inc_byte`/`dec_byte`, `inc_word`/`dec_word`, `inc_dword`/`dec_dword` | ramps | generic; dword ramp spots shifted sectors |
| `random` (default) | 32 KB random, tiled | most realistic; reproducible via `--seed` |

---

## Mode 2 — Single command

```
sudo ./build/DiskIOStress [opts] <dev> <subcmd> [args…]    # numeric args are hex
```

| Subcmd | Description | Args |
|--------|-------------|------|
| `w` / `r` / `t` | single write / read / trim | `[lba] [len]` |
| `sw` / `sr` | sequential write / read | `[start] [end] [count]` |
| `rw` / `rr` | random write / read | `[start] [end] [count]` |
| `rst` | controller reset | `[loop]` |
| `log` | get log page (0x02 = SMART) | `[log id]` |
| `fwact` | firmware download + activate | `[action] [slot] [bin]` |

> Data ops (`w`/`r`/`t`/`sw`/`sr`/`rw`/`rr`) go through io_uring and work on any
> transport (NVMe/SATA/USB); `t` (trim) uses the generic `BLKDISCARD` ioctl.
> Admin subcommands (`rst`, `log`, `fwact`) are NVMe-only (`/dev/nvmeXnY`).

---

## Mode 3 — Power-cycle test (`pcwrite` / `pcscan`)

Verifies data integrity across **frequent power cycles**. A single thread keeps
many commands in flight via io_uring + `O_DIRECT` (block layer → works on NVMe,
SATA and USB alike). `pcwrite` runs the whole loop; `pcscan` is a one-shot verify.

```
sudo ./build/DiskIOStress /dev/sdb pcwrite \
     --ranges=0-1G --access=random --io-size=64K --qd=32 \
     --durability=plp --checkpoint-interval=1M \
     --cut-after=64M --graceful-ratio=0.5 --cycles=100 \
     --power-hook=./power-hook.example.sh
```

What one cycle does:

```
write tagged data (QD-deep)  →  call power-hook "cut-graceful"/"cut-ungraceful"
   →  call power-hook "restore"  →  wait for the device  →  pcscan verify  →  repeat
```

**When the cut happens (graceful vs ungraceful):**

- **graceful** — write `--cut-after` bytes, **drain and flush** every write, advance
  the durable point to cover everything, *then* cut. The cut lands at a quiescent
  point with no IO outstanding, so nothing should be lost (the at-risk window is
  empty → `acceptable` is 0). This tests an orderly shutdown.
- **ungraceful** — a dedicated **cutter thread** fires the cut hook *while the IO
  pump keeps streaming writes*, so power physically drops with `~qd` writes
  genuinely **in flight** (true mid-IO cut), not at a boundary. The host stops
  feeding only when the device dies. This is what exercises torn / partial-program
  behaviour. The earlier design cut only *after* the write phase had fully drained,
  so it never actually interrupted an in-flight IO — that gap is now closed.

**Reproducibility of an ungraceful cut:** the *trigger* is deterministic — the cut
fires after exactly `--cut-after` submitted bytes — and the at-risk set
`[durable_units, submitted_units)` is written to the journal *before* each IO is
issued, so the pass/fail verdict is fully replayable. What is *not* host-controllable
is which sector the controller happens to be mid-program on at the instant the relay
opens; that is physical. (A dry-run confirms determinism: every cycle reports the
same `acceptable` count.)

| Option | Meaning |
|--------|---------|
| `--config=PATH` | load a power-cycle INI (see `powercycle.conf.example`); CLI overrides it |
| `--ranges=` | `whole`, or `0-1G,4G-5G` (byte ranges, K/M/G/T suffix) |
| `--regions=N --region-size=` | N equal regions spread across the device |
| `--access=seq\|random` | sequential-cyclic, or full-coverage random per pass |
| `--io-size=` | write unit (default 64K) |
| `--qd=` | queue depth held by the one thread (default 32) |
| `--durability=volatile\|plp` | durability model the test asserts (see below) |
| `--flush-interval=` | (volatile) flush + checkpoint cadence |
| `--checkpoint-interval=` | (plp) **bytes** of completed writes between durable-point journal persists (default 1M) |
| `--cut-after=` | bytes written per cycle before a cut |
| `--graceful-ratio=` | fraction of cuts that are graceful (0..1) |
| `--cycles=N` | number of cycles (0 = infinite) |
| `--power-hook=` | your power-control script (see `power-hook.example.sh`) |
| `--power-hook-dry-run` | run the loop without actually cutting power |
| `--no-direct` | disable `O_DIRECT` (e.g. testing on a regular file) |

> Size-valued options (`--io-size`, `--region-size`, `--flush-interval`,
> `--checkpoint-interval`, `--cut-after`, and the `--ranges` bounds) take a **byte
> count** with an optional `K`/`M`/`G`/`T` suffix (1024-based) or `0x` hex — e.g.
> `1M`, `262144`, `0x40000`.

### How verification works

Each sector's tag carries a **generation** counter. A *pass* rewrites every unit
in the range set exactly once (sequential, or a full-coverage random
permutation), then the generation increments — so at any moment a region holds at
most two live generations (`G` and `G-1`). A host-side **journal** (written
atomically, *off* the device under test, so it survives the cut) records the
generation and how far the pass got durably:

```
pos < durable_units                 → must be generation G   (durable: must survive)
durable_units ≤ pos < submitted     → at-risk window         (G or G-1 acceptable)
pos ≥ submitted                     → not yet written this pass (expect G-1)
```

After power returns, `pcscan` reads back every unit and tallies each as **newest**
(G), **prev** (G-1, expected), **acceptable** (in-flight loss inside the at-risk
window), **stale** (durable data lost — a failure), or **corrupt** (magic / CRC /
LBA mismatch = torn write or media damage). The test **passes iff
`stale == 0 && corrupt == 0`**.

### Durability models

* **`volatile`** — only *flushed* data is durable; the at-risk (acceptable-loss)
  window is everything written since the last flush (`--flush-interval`).
* **`plp`** — for power-loss-protected (capacitor-backed) SSDs: every **completed**
  write must survive. The durable point follows the io_uring completion prefix and
  is persisted every `--checkpoint-interval`, so the only acceptable-loss window is
  the writes truly in-flight at the cut (≈ `--qd`). A completed write that is lost
  or reverts to an older generation is a **failure** — i.e. the test validates the
  PLP actually works. Smaller `--checkpoint-interval` = stricter (more fsyncs).

### Power-control plugin

`pcwrite` calls your external script at each event. Contract (see
`power-hook.example.sh`, and `power-hook.usb-sim.sh` for a software-simulated USB
cut):

```
power-hook.sh <verb> <device>          # verb ∈ cut-graceful | cut-ungraceful | restore
   env: DIOS_GENERATION DIOS_CYCLE DIOS_PHASE DIOS_TIMEOUT DIOS_DRY_RUN
   exit 0 = ok, non-zero = abort the run
```

> **Note:** a *real* ungraceful test needs hardware that actually cuts VBUS/power.
> `power-hook.usb-sim.sh` only toggles the USB `authorized` flag (logical
> disconnect) — enough to exercise the disappear→reappear→rescan loop, but it does
> **not** power-cycle the drive's cache, so it can't prove true ungraceful behaviour.

---

## Sector tags & integrity verification

Every written sector starts with a 32-byte tag; the chosen pattern fills the rest.
On read-back each field is checked, so failures are classified precisely instead
of a generic "compare error":

| Field | Detects |
|-------|---------|
| `magic` | sector never written / torn write |
| `lba` | misdirected read or write |
| `seed` | leftover data from a previous run |
| `write_loop` (generation) | **stale data** — right LBA, older generation |
| `thread_id` / `trunk_index` | cross-thread / cross-region corruption |
| `pattern` / `workload` | run-level sanity |
| `payload_crc` (CRC32) | bit-rot in the sector body |

On mismatch the offending sector is dumped to `*_bad.dat` with an
expected-vs-actual field table.

## Reproducibility (seed)

Every mode is single-threaded io_uring, and the `--seed` (printed at startup and
logged) is woven into all the pseudo-random choices — the random access order,
the mixed-alignment decision, and the random payload bytes. Consequences:

* **Same `--seed` + same config → byte-for-byte identical run** (fully
  reproducible; great for re-running a failure).
* **Different seeds explore different sequences** (run-to-run coverage).
* `--seed=0` (default) derives a seed from `time()` and prints it — copy that
  value back with `--seed=0x…` to reproduce that exact run.

Power-cycle records the seed in its journal, so `pcscan` replays the same access
order the writer used. (The `addr` pattern's payload is positional by design, so
only the *order* varies with the seed there; the `random` pattern varies both.)

## Project layout

```
include/   module headers (types, config, util, nvme_cmd, disk_io, pattern,
           workload, subcommand, uring_engine, range, journal, powercycle)
src/       implementation — one .c per module
tests/     Unity unit tests (tests/unity/ = vendored framework)
build/     build output (git-ignored)
DiskIOStress.conf.example   stress-mode config template
powercycle.conf.example     power-cycle (pcwrite/pcscan) config template
power-hook.example.sh       power-control hook template
power-hook.usb-sim.sh       software-simulated USB power-cycle hook
samples/                    ready-to-edit configs per test case + command-samples.html
introduction.html          architecture / status overview (open in a browser)
```

## Tests

`make test` runs the Unity suite (66 tests): hex/size parsing, config parsing,
data patterns + tag generation, the io_uring engine, range/permutation math, the
journal, and power-cycle classification.

## Contributing

Useful and want to contribute? Contact xinyu0123@gmail.com
