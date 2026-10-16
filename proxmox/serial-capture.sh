#!/bin/bash
# Capture VM 113's serial console (guest boot-arg serial=3: IOLog, [IGPU]
# lines, the ReimsGT watch) to a log file on the Proxmox host. Start it any
# time, before or after `qm start`; stop it with Ctrl-C.
#
# Why not a plain socat: Proxmox creates the socket at VM start and removes it
# at stop, so socat started before `qm start` exits at once (empty log, as on
# 2026-10-16), and one started later exits at the next VM stop. This waits
# for the socket and reconnects across VM restarts and guest reboots.
# The socket takes one reader: close the web console / `qm terminal` first.
set -u
vmid=${1:-113}
sock=/var/run/qemu-server/$vmid.serial0
log=/root/vm$vmid-serial-$(date +%Y%m%d-%H%M%S).log
note() { echo "[serial-capture $(date +%H:%M:%S)] $*" >&2; }

if ! qm config "$vmid" 2>/dev/null | grep -q '^serial0: socket'; then
    note "VM $vmid has no 'serial0: socket' (qm set $vmid -serial0 socket)"
    exit 1
fi
note "log: $log"
note "waiting for $sock (start the VM whenever ready)"
: > "$log"
while :; do
    if [ -S "$sock" ]; then
        others=$(ss -xp 2>/dev/null | grep -c "$vmid.serial0")
        [ "$others" -gt 1 ] && note "another reader may be attached to the socket (web console?); output can go there instead"
        note "connected"
        socat -u UNIX-CONNECT:"$sock" STDOUT | tee -a "$log"
        note "disconnected (VM stopped or rebooting); waiting to reconnect, $(wc -c < "$log") bytes so far"
    fi
    sleep 1
done
