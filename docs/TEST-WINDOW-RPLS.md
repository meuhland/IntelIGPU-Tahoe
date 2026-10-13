# RPL-S test window runbook

One short window with the iGPU attached to VM 113. Goal: first hardware run
of stage 3 (HDMI test pattern) and of Apple's accelerator on `a780`
(phase 6 start), stopping before the display commit. Everything here is
code-checked only; nothing below has run on hardware yet.

## Before the window (iGPU still on the host)

On the guest, from the repository root, at the commit that contains this file:

1. Re-prepare the TGL candidate for rpls (SKU bypass + GT1 L3 patches); see
   CLAUDE.md for moving the old candidate aside. Expect candidate UUID
   `89dc2f5292cda68ec64560e2a5fa0dcd`, then commit `deferred-runtime.json`
   and `manual-gate/deferred_uuid.hpp`.
2. Rebuild the rpls gate and DesktopLink (they now check `RCSReady`), and
   VideoDiscovery, with `REIMS_TARGET=rpls` (commands in
   [PORT-RPLS-UHD770.md](PORT-RPLS-UHD770.md)); recommit their receipts.
   `kHardwareWritesPorted` stays false, so DesktopLink refuses the takeover.
3. Build both bring-up variants, stage 3:
   `REIMS_DISPLAY_BRINGUP=1 REIMS_TARGET=rpls REIMS_BRINGUP_STAGE=3 bash display-bringup/build.sh`
   (dry run), and the same with `REIMS_BRINGUP_EXECUTE=1`. Keep both copies.
4. Install the rebuilt gate, DesktopLink and candidate as in
   [DEPLOY-SAME-HARDWARE.md](DEPLOY-SAME-HARDWARE.md) sections 4–5; approve
   any new kexts in System Settings before the window.
5. User: add `-allow3d` to OpenCore `boot-args`. Without it `probe` fails
   any real device ID ("Failing probe (-allow3d is NOT set in boot-arg)").
6. `bash scripts/test.sh`, then take a VM snapshot.

## In the window

Stop at the first unexpected result, save evidence, and roll back the snapshot.

| # | Step | Expect | Captures |
|---|---|---|---|
| 1 | Start VM 113 (hookscript hands the IGD to vfio-pci) | Boots; `GFX0` `device-id` `ffff0000`, only `IONDRVFramebuffer` | `ioreg -r -n GFX0 -l` |
| 2 | Load the stage-3 dry run, `read_bringup.py`, unload | `Planned`, 0 blocked writes | read_bringup output |
| 3 | Load the stage-3 execute build and leave it loaded | `OK`; test pattern at 3840x2160@30 on the HDMI monitor; `PLANE_SURFLIVE` = `0x01000000` | read_bringup output, photo of the monitor |
| 4 | `sudo ./igpu-start` | Prepares: accelerator starts hidden on `a780`, DesktopLink `RCSReady` = true, state `prepared`; then stops at the commit because the takeover is refused | full console output |
| 5 | Evidence | see below | |

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
