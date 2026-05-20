#!/usr/bin/env bash
# =============================================================================
# DiskIOStress power-control hook (template)
# -----------------------------------------------------------------------------
# Register this script with:
#     ./DiskIOStress pcwrite --power-hook=./power-hook.example.sh ...
#   (or in the config file:  power_hook = ./power-hook.example.sh)
#
# The tool calls this script as:
#     power-hook.sh <verb> <device>
#   i.e. the two most-used values are positional args (easy to test by hand:
#   `./power-hook.example.sh cut-ungraceful /dev/sdb`). All remaining context is
#   passed via environment variables, so new fields can be added later without
#   breaking the positional contract. Implement the verbs your hardware supports.
#
# Positional arguments:
#   $1  verb       cut-graceful | cut-ungraceful | restore
#   $2  device     device under test, e.g. /dev/sdb or /dev/nvme0n1
#
# Environment variables provided on every call:
#   DIOS_GENERATION  current generation counter (monotonic)
#   DIOS_CYCLE       power-cycle iteration number (1, 2, 3, ...)
#   DIOS_PHASE       human-readable phase label (e.g. "pre-cut", "post-restore")
#   DIOS_TIMEOUT     seconds this hook should bound itself to
#   DIOS_DRY_RUN     "1" when invoked via --power-hook-dry-run (do NOT cut power;
#                    just echo what you would do and exit 0)
#
# Verb contract:
#   cut-graceful     Remove power AFTER the device has been allowed to settle.
#                    (The tool flushes + quiesces before calling this.)
#   cut-ungraceful   Remove power IMMEDIATELY to simulate a surprise yank.
#                    (The tool does NOT flush before calling this.)
#   restore          Re-apply power. Return 0 only once power is back on; the
#                    tool then polls for $DIOS_DEVICE to re-appear before scanning.
#
# Exit code:
#   0        success
#   non-zero abort the whole test run (the tool stops and reports)
# =============================================================================

set -euo pipefail

# Positional contract: $1 = verb, $2 = device.
VERB="${1:?missing verb (arg 1)}"
DEVICE="${2:?missing device (arg 2)}"
: "${DIOS_DRY_RUN:=0}"

log() { echo "[power-hook] gen=${DIOS_GENERATION:-?} cycle=${DIOS_CYCLE:-?} verb=$VERB dev=$DEVICE :: $*" >&2; }

# In dry-run we only describe the action so you can validate wiring safely.
if [[ "$DIOS_DRY_RUN" == "1" ]]; then
    log "DRY RUN — no power action taken"
    exit 0
fi

# ---------------------------------------------------------------------------
# Pick ONE implementation per verb for your rig and delete the rest.
# Examples below: uhubctl (USB), GPIO relay, network PDU, rtcwake (graceful proxy)
# ---------------------------------------------------------------------------

power_off() {   # immediate power removal (used by both cut verbs)
    # --- USB port via uhubctl (replace LOCATION/PORT for your hub) ---
    # uhubctl -l 1-1 -p 2 -a off

    # --- GPIO relay on the drive's power rail ---
    # gpioset gpiochip0 17=0

    # --- Network PDU outlet (replace URL/outlet) ---
    # curl -fsS -X POST "http://pdu.local/outlet/3/off" >/dev/null

    log "TODO: implement power_off for your hardware"
    return 1
}

power_on() {
    # uhubctl -l 1-1 -p 2 -a on
    # gpioset gpiochip0 17=1
    # curl -fsS -X POST "http://pdu.local/outlet/3/on" >/dev/null
    log "TODO: implement power_on for your hardware"
    return 1
}

case "$VERB" in
    cut-graceful)
        log "graceful cut"
        # Optional: device-specific safe quiesce before pulling power.
        # hdparm -F "$DEVICE" 2>/dev/null || true   # flush cache (SATA)
        sync
        power_off
        ;;

    cut-ungraceful)
        log "UNGRACEFUL cut (no flush)"
        power_off
        ;;

    restore)
        log "restore power"
        power_on
        # Hook returns once power is applied; the tool polls for the device node.
        ;;

    *)
        log "unknown verb: $VERB"
        exit 2
        ;;
esac

exit 0
