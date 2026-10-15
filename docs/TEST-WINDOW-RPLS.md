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

The host-side cold-GT experiment is done (2026-10-15): no host knob is
needed. The likely cause is Apple's standalone forcewake being a stub when
`getGPUInfo` runs ([PORT-RPLS-UHD770.md](PORT-RPLS-UHD770.md), "Likely root
cause"). Also build the forcewake-hold probe:

```sh
REIMS_TARGET=rpls REIMS_GT_FORCEWAKE_HOLD=1 bash gt-reset-probe/build.sh
ditto gt-reset-probe/build/ReimsGTResetProbe.kext ~/work/bringup/gt-hold/ReimsGTResetProbe.kext
```

It prints `built ReimsGTResetProbe.kext (HOLD …)`. It is a new binary: load it
once before the window (iGPU still on the host, it refuses without the
target and changes nothing) so any approval/reboot is out of the way.

### Next window: forcewake hold, then `igpu-start`

After step 1 below (VM booted, IGD isolated), before stage 3 and `igpu-start`:

```sh
sudo rm -rf /private/tmp/ReimsGTResetProbe.kext
sudo ditto ~/work/bringup/gt-hold/ReimsGTResetProbe.kext /private/tmp/ReimsGTResetProbe.kext
sudo chown -R root:wheel /private/tmp/ReimsGTResetProbe.kext
sudo kmutil load -p /private/tmp/ReimsGTResetProbe.kext
python3 gt-reset-probe/read_gt_reset.py
```

| Probe says | Meaning | Then |
|---|---|---|
| acks true, `Held` 1/3/`0xe00fa`, `held fuses populated: True` | the guest GT wakes; the stub was the problem | leave it **loaded**, continue with stage 3 and `igpu-start`; expect real `HWCAPS` (NumSlices 1, NumSubSlices 4, EU 32) and see whether `start` gets past the stamp wait |
| acks true, `Held` zero | GT awake by its own report but no fuses: a VM-only state | stop; save the output; GOP route back on the table |
| acks false | the GT cannot wake in the guest | stop; save the output |

Do not unload the probe while the accelerator is running: unloading releases
the hold.

**Result 2026-10-15:** first row. Held fuses `1`/`3`/`0`/`0xe00fa` (both acks
true), and HWCAPS then reported the real topology (1 slice, 4 sub-slices, 32
EU, L3 4). `igpu-start` still hung after `Scheduler Throttle Cap = 100ms.`,
silently (no reset, timeout or panic in 3–4 minutes). Details: PORT doc
"Forcewake hold on hardware". Stage 3 was skipped (see below); it is not
needed for the accelerator tests.

### Next window: watch the hang

Same sequence as above (hold probe, then `igpu-start` with candidate
`3a7cfb99`), with a rebuilt hold probe that now also runs the GT watch
([PORT-RPLS-UHD770.md](PORT-RPLS-UHD770.md), "GT watch for the hang"): while
loaded it prints the GT registers to serial every 250 ms when they change
(5 s heartbeat), and a `ReimsGT stall` line if its sampler gets stuck in a
register read. It is a new binary (it now also links `com.apple.kpi.mach`),
so build it and pre-load it once before the window, as above.

**Result 2026-10-15 evening:** the watch armed (`ReimsGTWatch: True`) but
printed nothing to serial: it used `kprintf`, which on a release kernel only
reaches serial with `debug=0x8`, otherwise only the (blocked) unified log.
The host MSI check worked: MSI enabled, `vfio-msi[0]` count 0 throughout. Fixed
to print with `IOLog` (PORT doc "Why the watch printed nothing"); **rebuild
the hold probe** (new binary: pre-load once) before rerunning this section.

**Result 2026-10-15 late:** the IOLog watch printed, but every line was cut
at 255 characters (the console's line buffer), losing the error columns. What
survived answers the questions: `ACTHD` never moved, `GT_INTR_DW0` stayed 0,
the master bit was set: **never executed**, and RCS flagged a catastrophic
error (`RING_RESET_CTL` `CAT_ERROR`) the moment `start` submitted (PORT doc
"GT watch on hardware: never executed"). The watch now prints each sample as
`@<column>` parts of 10 registers and samples the error, fault and execlist
registers too; **rebuild the hold probe again** (pre-load once).

Next run of this section, additions:

- Each sample now shows on serial as four `ReimsGT <seq> <ms> @0/@10/@20/@30 …`
  lines (and the header as four `ReimsGT seq ms @… names` lines). None may
  be cut: if a line ends mid-value, report it.
- Host, at three points: right after `qm start`, during the hang (after
  `igpu-start`), and after `qm stop`:
  `dmesg -T | grep -iE 'dmar|iommu|vfio|00:02.0|igpu-hookscript' | tail -40`, plus
  `date` each time, so every fault can be placed. Read the fault reason:
  `0x02` "Present bit in context entry is clear" means the device had no
  IOMMU domain at that moment (vfio bound without QEMU, i.e. before start or
  after stop), so it says the GPU was still writing then, not that the VM's
  mapping is wrong. `0x05`/`0x06` (page-table read/write) with an address
  *during* the hang would mean a GGTT entry points outside guest RAM.
  2026-10-15: one `DMA Write … fault addr 0x0 [fault reason 0x02]`, time
  not yet placed. A write to 0 in the VM would land silently in guest RAM at
  0; the watch's new `hwsPteLo/Hi` (the status page's GGTT entry) and
  `ggttPte0Lo/Hi` columns show whether the GPU's writes are aimed at physical 0.

1. Host: serial capture running (VM-PREP-RPLS.md 9b; only one reader on the
   socket).
2. Guest: load the hold probe, `read_gt_reset.py` (expect the first row
   above and `ReimsGTWatch: True`). **Check the capture now**: the
   `ReimsGT seq ms @0 …` header parts and a first `ReimsGT 1 … @0 …` sample
   must appear within a second of the load, before `igpu-start`. If they do not, stop:
   the watch output is still not reaching serial (then add `debug=0x8` to
   the boot args as a fallback, which enables `kprintf` too). Then
   `igpu-start`.
3. When `igpu-start` hangs, leave it for 60 s so the watch records the
   steady state, then `Ctrl-C` and save the capture. Do not unload the probe
   if the log shows a `ReimsGT stall … reading` line (the unload would touch
   the same stuck BAR); reboot the guest instead.
4. Decode on the Proxmox host, where the capture is (the script needs only
   Python 3's standard library):
   `curl -fsSL -o /root/read_gt_serial.py https://raw.githubusercontent.com/meuhland/IntelIGPU-Tahoe/uhd770-port/display-bringup/read_gt_serial.py`,
   then `python3 /root/read_gt_serial.py /root/vm113-serial-<ts>.log --msi flat` (or
   `--msi rising` if the host count moved this time). It ends with a "Hang
   verdict (RCS)": `ACTHD` moved or not, `GT_INTR_DW0`, the master IRQ bit,
   the RCS interrupt enable and `RING_IMR`, and one conclusion line (never
   submitted / never executed / masked at the master / raised but not
   reaching the MSI / masked at the engine / no interrupt requested). The
   summary above it still covers a stuck read or a starved sampler.

Also record the VM's vCPU count (`qm config 113 | grep -E 'cores|sockets'`);
it matters if the watch reports `starved`.

**Interrupt delivery, from the host (no new code).** Apple's standalone
accelerator takes the IGD's MSI (index 1; PORT doc "Standalone interrupt
path"). vfio shows the passed-through device's interrupts in the host's
`/proc/interrupts`, so on the Proxmox host:

```sh
grep -E 'vfio.*0000:00:02.0' /proc/interrupts                # before igpu-start (baseline)
lspci -vv -s 00:02.0 | grep -A1 'MSI:'                       # Enable+ once the guest enabled MSI
grep -E 'vfio.*0000:00:02.0' /proc/interrupts; sleep 5; grep -E 'vfio.*0000:00:02.0' /proc/interrupts   # during the hang
```

| Host shows during the hang | Meaning |
|---|---|
| a `vfio-msi` line whose count rises | the GPU raises MSIs and the host forwards them; delivery works |
| a `vfio-msi` line, count fixed (and the watch shows `GT_INTR_DW0` pending) | the GPU raised one interrupt that was never serviced, or never raised one: compare with the watch |
| no `vfio-msi` line (only `vfio-intx`, or nothing), `MSI: Enable-` | the guest never enabled MSI on the device: Apple's filter source is not attached or IOPCIFamily did not set MSI up |

## In the window

Stop at the first unexpected result and save evidence. A hang in step 4 only
needs `Ctrl-C` (that stops the `igpu-start` parent; its worker and the wedged
accelerator stay) and a guest reboot. Roll back the snapshot only if the disk
state is in doubt; a rollback also reverts any kexts installed since it.

**Stage-3 GGTT range.** Until 2026-10-15 the bring-up used a fixed GGTT
`0x01000000`. On the 2026-10-14 boots that range already held a mapping, most
likely host i915's 4K framebuffer left in the GGTT across the host→VM handoff
(host scanned from `0x00aa4000`; 8100 pages end at `0x02a48000`, and on
2026-10-15 the search landed on the next 256 KiB boundary, `0x02a80000`), so
the in-use guard stopped steps 2–3 at their first check
(`Scanout FAIL 0x808008`). The bring-up now uses the first unused 256 KiB-aligned
range from 16 MiB up inside GMADR (BAR2), and restores the range's entries on
unload and on any failed run. `read_bringup.py` prints the chosen
`ReimsBringupFramebufferGGTT` and `ReimsBringupGMADRLength`; it refuses with
`no unused GGTT range inside the GMADR aperture` if nothing fits. These are
new binaries (first load costs a reboot each). Run on 2026-10-15: dry run
`Planned` at `0x2a80000`, execute `OK`, pattern at 4K@30, EDID `OK`. The sampler's `fbPteLo`/`fbPteHi` watch stays fixed at BAR0 `0x808000` (GGTT `0x01000000`), so it follows the bring-up's first page only when `ReimsBringupFramebufferGGTT` is `0x1000000`.
A stage-2-only execute build (`REIMS_BRINGUP_STAGE=2 REIMS_BRINGUP_EXECUTE=1`)
did not change the accelerator's fuse reads or the hang (2026-10-14).

| # | Step | Expect | Captures |
|---|---|---|---|
| 1 | Start VM 113 (hookscript hands the IGD to vfio-pci) | Boots; `GFX0` `device-id` `ffff0000`, only `IONDRVFramebuffer` | `ioreg -r -n GFX0 -l` |
| 2 | Load the stage-3 dry run, `read_bringup.py`, unload | `Planned`, 0 blocked writes | read_bringup output |
| 3 | Load the stage-3 execute build and leave it loaded | `OK`; test pattern at 3840x2160@30 on the HDMI monitor; `PLANE_SURFLIVE` = `ReimsBringupFramebufferGGTT` | read_bringup output, photo of the monitor |
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

Keep the host serial capture running (VM-PREP-RPLS.md step 9b). Apple's own
`[IGPU]` lines are the main readout: on 2026-10-14 and 2026-10-15 they named
the zero fuses and the last line before the hang, while the sampler printed
nothing during the hang. After step
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
