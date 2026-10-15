# Linux control run: does the passed-through IGD execute in a VM at all?

Purpose: split the macOS start hang into "Apple's submission" versus "this VM
setup". In VM 113, Apple's first execlist submission makes the render engine
(RCS) flag `CAT_ERROR`, restore no ring, and issue a DMA write to address 0
(PORT-RPLS-UHD770.md, "GT watch on hardware" and after). Here the **same
passthrough** (same QEMU, vfio-pci, OVMF without GOP, IGD at guest `00:02.0`)
runs Linux's i915, which knows this GPU and reports hangs in detail.

i915 is run twice:

- **A: `i915.enable_guc=0`**: execlist submission by the host, the same
  hardware path as Apple's forced host scheduler (`GraphicsSchedulerSelect`
  5: `RING_EXECLIST_SQ_CONTENTS` `0x2510` + `EXECLIST_CONTROL` `0x2550`).
- **B: default**: GuC submission, which i915 uses on ADL-S/RPL-S (host log:
  `tgl_guc_70.bin`, "GUC: submission enabled").

No macOS rebuild is needed, and nothing is installed on the host. The cost is
one iGPU window (the hookscript stops the `/dev/dri` containers as for VM 113),
about 20–30 minutes.

## 1. Prepare (iGPU still on the host)

1. **ISO:** Ubuntu 24.04 LTS (or newer) desktop live ISO in a Proxmox ISO
   storage. It ships i915, the GuC/HuC firmware and Mesa.
2. **VM 114**, a copy of VM 113's hardware without disks:

   ```sh
   qm create 114 --name igpu-linux-control --machine q35 --bios ovmf \
     --memory 8192 --cores 4 --cpu host --vga std --net0 virtio,bridge=vmbr0 \
     --ide2 local:iso/<ubuntu-desktop.iso>,media=cdrom --boot order=ide2 \
     --efidisk0 local-lvm:1,efitype=4m,pre-enrolled-keys=0
   qm set 114 --hookscript local:snippets/igpu-hookscript.sh
   qm set 114 -serial0 socket
   ```

   Then copy VM 113's `args:` line, which attaches the IGD at `00:02.0` with
   raw `-device vfio-pci,host=0000:00:02.0,bus=pcie.0,addr=0x2` and pins
   `qemu-xhci` elsewhere (PORT doc "Proxmox guest setup"):

   ```sh
   qm config 113 | grep '^args:'
   qm set 114 --args '<the same value>'
   ```

   Keep `vga: std` (the console). The IGD is a secondary GPU, as in VM 113.
   `pre-enrolled-keys=0` keeps Secure Boot off so the live ISO boots. Adjust
   `local`/`local-lvm`/`vmbr0` to your storage and bridge names, and match
   VM 113's machine type if it pins a version (`qm config 113 | grep machine`).
3. Check the hookscript sees VM 114: `qm config 114 | grep hookscript`.

## 2. Run A: execlists (`i915.enable_guc=0`)

1. Host, second shell: serial capture loop (VM-PREP-RPLS.md 9b) with
   `113` replaced by `114`; host `date` and
   `dmesg -T | grep -iE 'dmar|vfio|00:02.0' | tail -5` as a baseline.
2. `qm start 114`, open the console. At the GRUB menu press `e`, append
   `i915.enable_guc=0` to the `linux` line (keep `quiet splash` or drop it),
   boot with Ctrl-X, choose "Try Ubuntu".
3. In a terminal:

   ```sh
   sudo dmesg | grep -iE 'i915|drm|dmar|guc' | head -60
   ls -l /dev/dri/by-path/
   cat /sys/module/i915/parameters/enable_guc        # expect 0
   sudo apt update && sudo apt install -y mesa-utils intel-gpu-tools
   ```

   i915 should bind `0000:00:02.0` (`Found alderlake_s/raptorlake_s ... a780`).
   Note any `stolen`, `GuC`, `GPU HANG`, `CAT`, `reset` lines.
4. Render-engine work on the IGD (the console runs on the emulated VGA, so
   ask Mesa for the Intel device explicitly):

   ```sh
   DRI_PRIME=1 glxinfo -B | grep -iE 'renderer|vendor'   # expect Mesa Intel(R) Graphics (RPL-S)
   DRI_PRIME=1 timeout 20 glxgears
   sudo intel_gpu_top -o - -s 1000 & sleep 5; DRI_PRIME=1 timeout 10 glxgears; sudo pkill intel_gpu_top
   ```

   If `DRI_PRIME` picks the wrong device, list them with
   `DRI_PRIME=1 vulkaninfo --summary` (package `vulkan-tools`) or use
   `DRI_PRIME=pci-0000_00_02_0`.
5. Engine state (the counterpart of our watch):

   ```sh
   sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null
   sudo cat /sys/kernel/debug/dri/*/i915_engine_info > ~/engine-info-A.txt
   sudo cat /sys/kernel/debug/dri/*/i915_sseu_status > ~/sseu-A.txt
   sudo dmesg > ~/dmesg-A.txt
   for e in /sys/class/drm/card*/error; do sudo cat "$e" > ~/error-A-$(basename $(dirname $e)).txt; done
   ```

   Copy the files off (a USB stick, `scp` to the host, or paste the
   interesting parts). The `error` file is i915's GPU error state: empty
   ("No error state collected") unless a hang occurred.
6. Host: `date; dmesg -T | grep -iE 'dmar|vfio|00:02.0' | tail -20`. Any
   `DMAR` fault naming `00:02.0` during run A matters.

## 3. Run B: GuC (default)

Reboot the guest (no GRUB edit), repeat steps 3–6 with `-B` file names;
`enable_guc` reads `-1` or `3` here.

## 4. Stop and hand back

`qm stop 114`; the hookscript returns the IGD to the host's i915 and restarts
the containers (check `dmesg` for i915 on the host as after VM 113).

## 5. Reading the result

| Run A (execlists) | Run B (GuC) | Meaning | Next |
|---|---|---|---|
| `glxgears` runs, no hang, no DMAR | either | execlist submission works on this IGD in this VM | the macOS failure is in what Apple submits or how it maps it (context image/LRCA/GGTT): the macOS context dump decides |
| `GPU HANG` / reset, CAT error or DMAR fault | works | execlists from a VM without firmware init break, GuC submission does not | try Apple's GuC scheduler in macOS (profile `GraphicsSchedulerSelect` default instead of 5) |
| hangs | hangs | the VM setup itself breaks command execution | firmware route (OVMF IgdAssignmentDxe + GOP); no bare-metal or ADL-P reference is available |
| i915 fails to probe or to load GuC | | i915 reports why in `dmesg` | read the reason before anything else |

This run is also the project's only known-good reference on this hardware
(no ADL-P machine and no bare-metal macOS boot are available). Compare run
A's `i915_engine_info` for `rcs0` with our watch: `RING_START`,
`RING_CTL` (valid), `ACTHD`, `CCID`, execlist status and the CSB pointers after
real work. They show what a working Gen12 RCS looks like on this exact
hardware and VM.

## Notes

- Nothing here writes to the host. The live session leaves no disk behind;
  delete VM 114 afterwards if wanted (`qm destroy 114`).
- If the Ubuntu live session will not start its desktop, the text console
  (Ctrl-Alt-F3) is enough: use `kmscube` (`sudo apt install kmscube`,
  `kmscube -D /dev/dri/cardN` with the i915 card) or the IGT test
  `gem_exec_basic` if `intel-gpu-tools` ships it
  (`ls /usr/libexec/*gpu-tools*`).
