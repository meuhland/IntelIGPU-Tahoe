# RPL-S test window runbook

One short window with the iGPU attached to VM 113. Goal: first hardware run
of stage 3 (HDMI test pattern) and of Apple's accelerator on `a780`
(phase 6 start), stopping before the display commit. Everything here is
code-checked only; nothing below has run on hardware yet.

## Before the window (iGPU still on the host)

Build, install and check everything as in [VM-PREP-RPLS.md](VM-PREP-RPLS.md):
re-prepared TGL candidate, rpls gate/DesktopLink/VideoDiscovery, both stage-3
bring-up builds, the Metal bundle, `-allow3d` in the boot args, then a VM
snapshot.

## In the window

Stop at the first unexpected result, save evidence, and roll back the snapshot.

| # | Step | Expect | Captures |
|---|---|---|---|
| 1 | Start VM 113 (hookscript hands the IGD to vfio-pci) | Boots; `GFX0` `device-id` `ffff0000`, only `IONDRVFramebuffer` | `ioreg -r -n GFX0 -l` |
| 2 | Load the stage-3 dry run, `read_bringup.py`, unload | `Planned`, 0 blocked writes | read_bringup output |
| 3 | Load the stage-3 execute build and leave it loaded | `OK`; test pattern at 3840x2160@30 on the HDMI monitor; `PLANE_SURFLIVE` = `0x01000000` | read_bringup output, photo of the monitor |
| 4 | `sudo ./igpu-start` | Prepares: accelerator starts hidden on `a780`, DesktopLink `RCSReady` = true, state `prepared`; then stops at the commit because the takeover is refused | full console output |
| 5 | Evidence | see below | |

Commands for steps 2–4 (kmutil wants a root-owned copy):

```sh
# 2: dry run
sudo rm -rf /private/tmp/ReimsDisplayBringup.kext
sudo ditto ~/work/bringup/stage3-dryrun/ReimsDisplayBringup.kext /private/tmp/ReimsDisplayBringup.kext
sudo chown -R root:wheel /private/tmp/ReimsDisplayBringup.kext
sudo kmutil load -p /private/tmp/ReimsDisplayBringup.kext
python3 display-bringup/read_bringup.py
sudo kmutil unload -b lab.reims.ReimsDisplayBringup

# 3: execute build, left loaded (unloading turns the plane off)
sudo rm -rf /private/tmp/ReimsDisplayBringup.kext
sudo ditto ~/work/bringup/stage3-execute/ReimsDisplayBringup.kext /private/tmp/ReimsDisplayBringup.kext
sudo chown -R root:wheel /private/tmp/ReimsDisplayBringup.kext
sudo kmutil load -p /private/tmp/ReimsDisplayBringup.kext
python3 display-bringup/read_bringup.py

# 4
sudo ./igpu-start
```

At step 4, the first Apple driver run on RPL-S, look for:

- `probe` passes: no "Failed to map Device ID" panic and no `-allow3d` refusal.
- `getGPUInfo` log: 1 slice, 4 subslices, 32 EUs, L3BankCount 4, and no
  "Invalid NumSlice or NumSubSlices".
- Whether the test pattern survives the accelerator start (open question in
  [PHASE5-HANDOFF-AUDIT.md](PHASE5-HANDOFF-AUDIT.md): it may reprogram the
  GGTT or display state).
- Engine resets or a `userspace watchdog timeout` (NootedGreen saw both on
  RPL-P): capture and stop; do not retry in the same boot.

Evidence to save before rolling back:

```sh
log show --last 10m --predicate 'sender == "AppleIntelTGLGraphics" OR sender == "ReimsADLDesktopLink" OR sender == "ReimsDisplayBringup"' > ~/work/igpu-notes/window-log.txt
ioreg -r -n GFX0 -l -a > ~/work/igpu-notes/window-gfx0.plist
```

## After the window

Return the iGPU to the host (stop the VM; the hookscript rebinds i915).
Record results in CLAUDE.md and the port doc. Enabling the takeover
(`kHardwareWritesPorted` for rpls) is a separate review decision once step 4
works: it is the only rpls path still behind that flag.
