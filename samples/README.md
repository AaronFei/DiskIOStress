# Sample configs

Ready-to-edit config files for common test cases. Open
[`command-samples.html`](command-samples.html) in a browser for copy-paste
command examples.

> ⚠️ Every run **destroys data** on the target device. Set the right `/dev/...`.

## `no-power/` — stress mode (no power events)

Load with `--config=` and pass the device last:

```
sudo ./build/DiskIOStress --config=samples/no-power/<file>.conf /dev/sdX
```

| File | What it does |
|------|--------------|
| `seq-verify.conf`    | sequential write → read → verify (64K) |
| `rand-verify.conf`   | random write → read → verify (64K) |
| `mix-rw.conf`        | write + re-read/verify ×2 (read-heavy mix) |
| `4k-aligned.conf`    | 4 KiB I/O, every command 4K-aligned |
| `4k-unaligned.conf`  | 4 KiB I/O, every command offset 1 sector (4K-unaligned) |
| `4k-mixed.conf`      | 4 KiB I/O, per-command mix of aligned + unaligned |
| `large-cmd.conf`     | 1 MiB transfers (max-size commands) |
| `trim-mixed.conf`    | write/verify then TRIM each pass |

## `with-power/` — power-cycle mode (pcwrite)

Load with `pcwrite --config=`:

```
sudo ./build/DiskIOStress /dev/sdX pcwrite --config=samples/with-power/<file>.conf
```

| File | What it does |
|------|--------------|
| `seq-graceful.conf`     | sequential, graceful cuts only, volatile |
| `rand-ungraceful.conf`  | random, ungraceful (surprise) cuts, volatile |
| `plp-strict.conf`       | PLP assertion: completed writes must survive |
| `4k-mixed-cuts.conf`    | 4K random, mixed graceful/ungraceful, PLP |
| `large-cmd.conf`        | 1 MiB commands across power cuts |

The `power_hook` defaults to `./examples/power-hook.usb-sim.sh` (a *simulated* USB
disconnect — not a real VBUS cut). Point it at your real power-control script
for true ungraceful testing.

CLI options always override config keys, e.g. add `--cycles=50` or
`--power-hook=./my-relay.sh`.
