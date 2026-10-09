#!/bin/bash
# Proxmox VM hookscript (host side, not run in the guest). The IGD is attached
# with raw args:, so Proxmox does not rebind it. Give it to vfio-pci while the
# VM runs and back to the host driver (i915, used for LXC QuickSync) after.
# Install: copy to /var/lib/vz/snippets/igpu-switch.sh, chmod +x, then
#   qm set <vmid> --hookscript local:snippets/igpu-switch.sh
dev=0000:00:02.0
sys=/sys/bus/pci/devices/$dev
case "$2" in
pre-start)
  drv=$(basename "$(readlink $sys/driver 2>/dev/null)")
  [ "$drv" = vfio-pci ] && exit 0
  # Refuse rather than pull the GPU from an active host or LXC user.
  if fuser -s /dev/dri/by-path/pci-$dev-card /dev/dri/by-path/pci-$dev-render 2>/dev/null; then
    echo "iGPU is in use on the host (LXC transcode?); stop it first" >&2; exit 1
  fi
  [ -n "$drv" ] && echo $dev > $sys/driver/unbind
  modprobe vfio-pci
  echo vfio-pci > $sys/driver_override
  echo $dev > /sys/bus/pci/drivers_probe
  ;;
post-stop)
  echo > $sys/driver_override
  [ -e $sys/driver ] && echo $dev > $sys/driver/unbind
  echo $dev > /sys/bus/pci/drivers_probe
  ;;
esac
exit 0
