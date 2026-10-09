#!/bin/bash
# Proxmox VM hookscript: lend the Intel iGPU to this VM while it runs, and give
# it back to the host (i915) and its LXC containers when the VM stops.
#
# Install on the Proxmox host:
#   /var/lib/vz/snippets/igpu-hookscript.sh  (chmod 755)
#   qm set <vmid> --hookscript local:snippets/igpu-hookscript.sh
# Requires: i915/xe NOT blacklisted and 8086:a780 NOT in vfio-pci ids=, so the
# host owns the iGPU (Quick Sync for containers) while the VM is off.
# Dry run without the VM: igpu-hookscript.sh <vmid> pre-start / post-stop

set -u

IGPU=0000:00:02.0
# Containers to stop while the VM owns the iGPU. Empty: every container whose
# config passes through /dev/dri.
LXC_IDS=""
RENDER_WAIT=20
STATE_DIR=/run/igpu-hookscript

DEV=/sys/bus/pci/devices/$IGPU
vmid=${1:?usage: $0 <vmid> <phase>}
phase=${2:?usage: $0 <vmid> <phase>}

log() {
    echo "igpu-hookscript[$vmid/$phase]: $*"
    logger -t igpu-hookscript "[$vmid/$phase] $*"
}

current_driver() {
    if [ -e "$DEV/driver" ]; then basename "$(readlink -f "$DEV/driver")"; else echo none; fi
}

dri_containers() {
    if [ -n "$LXC_IDS" ]; then echo "$LXC_IDS"; return; fi
    grep -l '/dev/dri' /etc/pve/lxc/*.conf 2>/dev/null | xargs -r -n1 basename | sed 's/\.conf$//'
}

restart_containers() {
    local file=$STATE_DIR/$vmid.lxc
    [ -f "$file" ] || return 0
    while read -r id; do
        [ -n "$id" ] || continue
        log "starting CT $id"
        pct start "$id" || log "CT $id failed to start"
    done < "$file"
    rm -f "$file"
}

# fbcon on i915's framebuffer can stall the unbind; detach it first and
# reattach after i915 returns.
unbind_fbcon() {
    : > "$STATE_DIR/$vmid.fbcon"
    for vt in /sys/class/vtconsole/vtcon*; do
        [ -e "$vt/bind" ] || continue
        if grep -q 'frame buffer' "$vt/name" && [ "$(cat "$vt/bind")" = 1 ]; then
            echo 0 > "$vt/bind" && echo "$vt" >> "$STATE_DIR/$vmid.fbcon"
        fi
    done
}

rebind_fbcon() {
    local file=$STATE_DIR/$vmid.fbcon
    [ -f "$file" ] || return 0
    while read -r vt; do
        [ -e "$vt/bind" ] && echo 1 > "$vt/bind"
    done < "$file"
    rm -f "$file"
}

give_back() {
    if [ "$(current_driver)" = vfio-pci ]; then
        log "unbinding from vfio-pci"
        echo "$IGPU" > "$DEV/driver/unbind"
    fi
    echo > "$DEV/driver_override"
    echo "$IGPU" > /sys/bus/pci/drivers_probe
    local render=/dev/dri/by-path/pci-$IGPU-render i
    for ((i = 0; i < RENDER_WAIT; i++)); do
        [ -e "$render" ] && break
        sleep 1
    done
    rebind_fbcon
    if [ ! -e "$render" ]; then
        # Containers would fail on the missing device; leave them stopped.
        log "iGPU did not return to the host (driver: $(current_driver)); reboot the host to recover"
        rm -f "$STATE_DIR/$vmid.lxc"
        return 1
    fi
    log "iGPU back on $(current_driver) ($(readlink -f "$render"))"
    restart_containers
}

pre_start() {
    mkdir -p "$STATE_DIR"
    : > "$STATE_DIR/$vmid.lxc"
    local id drv nodes
    for id in $(dri_containers); do
        if pct status "$id" 2>/dev/null | grep -q 'status: running'; then
            log "stopping CT $id"
            if ! pct shutdown "$id" --timeout 60 && ! pct stop "$id"; then
                log "cannot stop CT $id; aborting VM start"
                restart_containers
                return 1
            fi
            echo "$id" >> "$STATE_DIR/$vmid.lxc"
        fi
    done

    drv=$(current_driver)
    if [ "$drv" = vfio-pci ]; then
        log "iGPU already on vfio-pci"
        return 0
    fi
    if [ "$drv" != none ]; then
        nodes=$(ls /dev/dri/by-path/pci-$IGPU-* 2>/dev/null)
        if [ -n "$nodes" ] && command -v fuser > /dev/null && fuser -s $nodes 2>/dev/null; then
            log "iGPU still in use: $(fuser -v $nodes 2>&1 | tail -n +2 | tr -s ' ' | tr '\n' ';')"
            restart_containers
            return 1
        fi
        unbind_fbcon
        log "unbinding from $drv"
        if ! echo "$IGPU" > "$DEV/driver/unbind"; then
            log "unbind failed"
            give_back
            return 1
        fi
    fi
    modprobe vfio-pci
    echo vfio-pci > "$DEV/driver_override"
    echo "$IGPU" > /sys/bus/pci/drivers_probe
    if [ "$(current_driver)" != vfio-pci ]; then
        log "bind to vfio-pci failed (driver: $(current_driver))"
        give_back
        return 1
    fi
    log "iGPU bound to vfio-pci"
}

case "$phase" in
    pre-start) pre_start || exit 1 ;;
    post-stop) give_back ;;
esac
exit 0
