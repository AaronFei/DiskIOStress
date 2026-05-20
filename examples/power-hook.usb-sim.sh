#!/usr/bin/env bash
# =============================================================================
# DiskIOStress power hook: SIMULATED USB power-cycle via sysfs authorized flag.
#
# This logically disconnects/reconnects the USB device so $DEVICE disappears and
# re-enumerates. It does NOT cut VBUS, so the drive's internal cache is NOT
# actually power-cycled -- use it to exercise the disappear -> reappear -> rescan
# loop, not to validate true ungraceful cache-loss behaviour.
#
# Contract:  power-hook.usb-sim.sh <verb> <device>
#   verbs: cut-graceful | cut-ungraceful | restore
# Must run as root (pcwrite already runs under sudo).
# =============================================================================
set -euo pipefail

VERB="${1:?missing verb}"
DEVICE="${2:?missing device}"
dev="$(basename "$DEVICE")"

# Walk up the sysfs tree from the block device to the USB device node that owns
# an 'authorized' attribute and is named like "2-4" / "2-4.1".
find_usb_node() {
    local p
    p="$(readlink -f "/sys/block/$dev")"
    while [ "$p" != "/" ] && [ -n "$p" ]; do
        local b; b="$(basename "$p")"
        if [[ "$b" =~ ^[0-9]+-[0-9]+(\.[0-9]+)*$ ]] && [ -f "$p/authorized" ]; then
            echo "$p"; return 0
        fi
        p="$(dirname "$p")"
    done
    return 1
}

log() { echo "[usb-sim] gen=${DIOS_GENERATION:-?} cycle=${DIOS_CYCLE:-?} $VERB $DEVICE :: $*" >&2; }

case "$VERB" in
    cut-graceful|cut-ungraceful)
        usb="$(find_usb_node)" || { log "cannot locate USB node"; exit 1; }
        [ "$VERB" = "cut-graceful" ] && sync
        log "deauthorize $usb"
        echo 0 > "$usb/authorized"
        ;;
    restore)
        # The device node is gone now; re-find via the parent hub is unreliable,
        # so reauthorize every currently-deauthorized USB device (cheap + safe in
        # a dedicated test rig). The tool then polls for $DEVICE to return.
        for a in /sys/bus/usb/devices/*/authorized; do
            if [ "$(cat "$a" 2>/dev/null)" = "0" ]; then
                log "reauthorize ${a%/authorized}"
                echo 1 > "$a" || true
            fi
        done
        ;;
    *)
        log "unknown verb"; exit 2 ;;
esac
exit 0
