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

**First load of each new kext needs a reboot.** On this guest `kmutil load -p`
of a never-before-loaded binary stages it to the AuxKC and prints "requires a
reboot" instead of loading it (same as the phase-4 probe). Reboot, then
re-`ditto` to `/private/tmp` (cleared on reboot) and load again — the second
load is live. The dry-run and execute builds share a bundle id but are
different binaries, so each costs one reboot the first time. The iGPU stays
passed through across a guest reboot and the snapshot is untouched.
`igpu-start` loads DesktopLink from `/Library/Extensions`, already in the boot
AuxKC, so it needs no reboot. Confirmed on hardware 2026-10-13 (stage 3 `OK`,
test pattern at 4K@30).

Keep the host serial capture running (VM-PREP-RPLS.md step 9b). After step
3 the console shows `ReimsGT seq ms <names…>` once, then a line whenever a
sampled register changes and a heartbeat every 5 s. If step 4 wedges again,
the last lines say where `startGraphicsEngine` stopped:

- `fwAckGT`/`fwAckRender` without the requested bits: forcewake never acked.
- `gucStatus` changing then stuck (and `gucDmaCtrl`): firmware load or
  scheduler init (the profile asks for the host scheduler, type 5).
- RCS/BCS head ≠ tail with `rcsActhd` frozen, or `rcsCsb`/`rcsExecStatus`
  stuck: the first stamp submissions never completed.
- `errorGen6`/`ringFault` nonzero: a GT error.
- `transconfA`, `surfLiveA` and `fbPteLo`/`fbPteHi` across the blanking:
  pipe turned off, plane moved, or the bring-up's GGTT entry rewritten
  (docs/HANDOFF-OPTIONS-RPLS.md uses this to pick a handoff option).
- Heartbeats stopping entirely: the CPU or work loops are blocked, not just
  IOKit (also worth knowing).

Save the host log with the window evidence, and decode it on any machine
with Python 3 (timeline of changes plus a final-state summary):

```sh
python3 display-bringup/read_gt_serial.py /root/vm113-serial-<timestamp>.log
```

### What NootedGreen saw on RPL-P, and how it would show here

NootedGreen (sgiammori, README at `95f8189`) runs the same TGL driver on an
RPL-P laptop under Sonoma. Its findings are claims from that README, not
verified here; this port never fakes completion, so they are symptoms to
recognise, not fixes to copy.

| NootedGreen finding (RPL-P) | In `read_gt_serial.py` output |
|---|---|
| Native forcewake acks do not arrive (it replaces `SafeForceWakeMultithreaded`; the profile here sets `MultiForceWakeSelect` 1) | "forcewake: bit 1 not acknowledged" while the accelerator is starting |
| GuC firmware init fails on RPL/ADL; it boots with `-disablegfxfirmware` because firmware init starts before its scheduler choice | `gucStatus` changes (boot ROM / microkernel fields) then freezes, or auth `BAD` |
| `resetGraphicsEngine` returns 1025 (timeout) on every call; resets repeat until a userspace watchdog panic about 120 s later | `rcsReset` toggling `request`/`ready`, RCS idle; the log keeps changing |
| BCS cannot execute under the TGL driver ("ring not empty" resets) | "BCS has queued work and ACTHD is not moving" |
| Execlist stalls after resets (`EXEC=0x40018098`, `CSB=0x1001`: status entries never consumed) | `rcsExecStatus`/`rcsCsb` frozen with the CSB write pointer ahead of the read pointer |
| The logical context's ring page is left empty, so a submitted context idles without going active | RCS "on, idle" with `rcsActhd` unchanged and the start still stuck: the stamp wait, not the ring |
| A stamp write lands in an unmapped GGTT page (its open question) | Same as above: ring idle, start still stuck |
| `ERROR_GEN6` raised (it masks it permanently) | "GT error registers nonzero" |

`-disablegfxfirmware` is an Apple option: the `sle` binary contains the
string next to `GraphicsSchedulerSelect`. If the first attempt points at
GuC firmware, the cheapest second attempt in the same window is to add it
to the OpenCore `boot-args`, reboot (the hang needs one anyway) and repeat
steps 3–4.

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
