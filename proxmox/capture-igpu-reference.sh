#!/bin/bash
# Read-only capture of the host i915 display state for the UHD 770 while i915
# drives a monitor on a motherboard output. Phase 5 reference for the port's
# own display bring-up (docs/PHASE5-DISPLAY-BRINGUP.md). Run on the Proxmox
# host as root with the macOS VM stopped (iGPU on i915). Needs intel-gpu-tools
# (intel_reg, intel_vbt_decode). Writes /root/igpu-reference-<timestamp>/.
# Reads sysfs/debugfs files and MMIO only; never writes a register.
set -u
dev=0000:00:02.0
out=/root/igpu-reference-$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"

drv=$(basename "$(readlink -f /sys/bus/pci/devices/$dev/driver 2>/dev/null)" 2>/dev/null)
if [ "$drv" != i915 ]; then
    echo "iGPU is on '${drv:-none}', not i915; stop the macOS VM first" >&2
    exit 1
fi
lspci -nnvvk -s "$dev" > "$out/lspci.txt" 2>&1

# Connectors on the iGPU: status, modes, EDID.
for c in /sys/class/drm/card*-*; do
    [ "$(readlink -f "$c/device" 2>/dev/null)" = "$(readlink -f /sys/bus/pci/devices/$dev)" ] || continue
    name=$(basename "$c")
    for f in status enabled modes; do
        [ -r "$c/$f" ] && { echo "== $f"; cat "$c/$f"; } >> "$out/connector-$name.txt"
    done
    [ -s "$c/edid" ] && cat "$c/edid" > "$out/edid-$name.bin"
done

# i915 debugfs state for this device.
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
dbg=
for d in /sys/kernel/debug/dri/*/; do
    grep -q "$dev" "$d/name" 2>/dev/null && { dbg=$d; break; }
done
if [ -n "$dbg" ]; then
    for f in i915_vbt i915_display_info i915_display_capabilities i915_shared_dplls_info \
             i915_power_domain_info i915_cdclk_info i915_ddb_info i915_dmc_info \
             i915_gem_framebuffer i915_frequency_info; do
        [ -r "$dbg/$f" ] && cat "$dbg/$f" > "$out/$f" 2>/dev/null
    done
    if command -v intel_vbt_decode > /dev/null && [ -s "$out/i915_vbt" ]; then
        intel_vbt_decode --file "$out/i915_vbt" > "$out/vbt-decoded.txt" 2>&1
    fi
else
    echo "no i915 debugfs entry for $dev" > "$out/debugfs-missing.txt"
fi

# Display MMIO, read-only. Blocks: power wells/DC/fuses, CDCLK and DBUF,
# DPLL enables and CFGCR, DPCLKA, DDI buffers A/TC1-TC4, transcoders A-D,
# pipes and planes A-D, TRANS_CLK_SEL, DSSM.
if command -v intel_reg > /dev/null; then
    {
        intel_reg read 0x42000 0x51004 0xc2014
        intel_reg read --count=24 0x45400
        intel_reg read --count=4 0x45500
        intel_reg read --count=32 0x46000
        intel_reg read --count=8 0x46140
        intel_reg read --count=4 0x45008 0x44fe8
        intel_reg read --count=16 0x164280
        intel_reg read --count=12 0x1642bc
        for ddi in 0x64000 0x64300 0x64400 0x64500 0x64600; do intel_reg read --count=4 $ddi; done
        for t in 0 1 2 3; do
            intel_reg read --count=24 $(printf '0x%x' $((0x60000 + t * 0x1000)))
            intel_reg read --count=12 $(printf '0x%x' $((0x60400 + t * 0x1000)))
            intel_reg read --count=16 $(printf '0x%x' $((0x70000 + t * 0x1000)))
            # Plane 1 through PLANE_WM_1 (0x70240) and PLANE_BUF_CFG_1 (0x7027c).
            intel_reg read --count=64 $(printf '0x%x' $((0x70180 + t * 0x1000)))
        done
    } > "$out/mmio.txt" 2>&1
else
    echo "intel_reg not installed (apt install intel-gpu-tools)" > "$out/mmio-missing.txt"
fi

echo "captured: $out"
ls -la "$out"
