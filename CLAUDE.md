# IntelIGPU-Tahoe — UHD 770 port

Fork of m4j2rpf766-crypto/IntelIGPU-Tahoe (GPL-3.0, docs mostly in Chinese):
experimental macOS Tahoe support for an Intel ADL-P laptop iGPU (8086:46a3 rev 0c)
built on Apple's Ventura 13.7.8 AppleIntelTGLGraphics 16.0.0. This fork ports it
to a desktop Raptor Lake-S UHD 770 (8086:a780 rev 04).

## Setup

- Branch `uhd770-port`. Remotes: `origin` = meuhland/IntelIGPU-Tahoe (fork),
  `upstream` = original. `gh` default repo is the fork.
- Never open PRs, issues or comments on upstream; the user handles contact
  with the original author.
- Commit identity (repo-local): Louis-Philippe Gauthier
  <6123610+meuhland@users.noreply.github.com>.
- Never commit Apple binaries (kexts, Metal bundles, KDK files). Keep them
  outside the checkout.

## Target machine

macOS Tahoe 25G83 guest on Proxmox (QEMU/KVM), iGPU passed through with VFIO;
host display runs on a dGPU. When the VM is off the host uses the iGPU for LXC
QuickSync, so never bind it to vfio-pci at host boot. Requirements and unknowns:
[docs/PORT-RPLS-UHD770.md](docs/PORT-RPLS-UHD770.md).

## Port state

Plan phases: 1 Linux recon, 2 identity, 3 ffff boot isolation, 4 read-only
display decode, 5 framebuffer takeover, 6 GT1 Metal bring-up, 7 desktop handoff,
8 media/HDMI/stability.

- Done (phase 2, commit f9681d1): identity checks centralized in
  `common/reims_target.hpp`; kernel builds select it via `REIMS_TARGET=rpls`
  (`scripts/target-flags.sh`); default `adlp` keeps upstream unchanged. On rpls,
  register-writing paths (native framebuffer takeover, RCS PSMI workaround) are
  refused; read-only probes remain.
- Compile-checked (SDK 26.5, Apple clang 21): the gate and VideoDiscovery build
  clean with `-Werror` for both targets. rpls binaries carry 0xa780/rev 0x04,
  `BAAAAA==` and the rpls path; `REIMS_PCI_PATH` overrides it. adlp matches the
  pre-port 9baa4bb build (identical strings and VideoDiscovery text; the gate
  only outlines `ReimsTarget::isExact`). Nothing loaded or run on hardware.
- TGL binary: no published KDK has it (22H730 does not exist; 22H722 lacks
  it). The port uses the community `sle_Internal/sle` copy (SHA-256
  `890735a9…27ac`), which passes every upstream offset and ABI check but not
  upstream's hash `ae99582b…7d47`; prepare-deferred-runtime.py accepts both.
  Details in docs/PORT-RPLS-UHD770.md. Source kext (binary + Info.plist):
  `~/work/tgl-candidates/sle/AppleIntelTGLGraphics.kext`.
- Prepared: deferred TGL candidate UUID `edbb0eb4…af9a`, kept outside the
  checkout at `~/work/tgl-candidates/ReimsTGLManualRuntime.kext` (the deploy
  doc's `ditto` source path must point there). Gate, DesktopLink and
  VideoDiscovery are built for rpls and all tracked `*-current.json` receipts
  describe those builds. Receipts are per build (debug maps embed object
  timestamps), so deploy these exact kexts or rebuild and recommit receipts.
  The gate does not compile `deferred_uuid.hpp`; session.py checks the TGL
  UUID via `deferred-runtime.json`.
- Metal files (`~/work/tgl-candidates/sle/*.bundle`): none match upstream's
  SHA-256 pins. MTLDriver is upstream's build (LC_UUID equals the pinned
  `tglUUID`; patch offsets line up); `libigdmd` and the VA driver
  (`NativeHEVCVA`) are unverified. Table in docs/PORT-RPLS-UHD770.md.
- Guest placement verified (VM 113, PVE 9.2 / QEMU 11.1.1): IGD at `00:02.0`
  via raw `args:` (Proxmox `hostpci` puts it behind a root port), host
  switches `i915` ↔ `vfio-pci` with `proxmox/igpu-hookscript.sh`
  because the host uses the IGD for LXC QuickSync. Guest path
  `.../PCI0/AppleACPIPCI/S10@2`; rpls default gate path assumes the OpenCore
  rename to `GFX0@2` (patch table in docs/PORT-RPLS-UHD770.md). Gate rebuilt.
- Guest OpenCore 1.0.7 (`/Volumes/EFI/EFI/OC/config.plist`, `ocvalidate`
  clean): `GFX0` rename verified, the IGD is at the gate's rpls default path;
  SIP `csr-active-config` = `01000000` (Kext Signing only). Claude runs on
  this guest. SIP/security settings are the user's to change.
- Done (phase 3): ffff isolation boot verified; `GFX0` registry `device-id`
  is `<ffff0000>`, `IONDRVFramebuffer` attached, no Intel graphics kext loaded.
  Config backups `config.plist.bak-20261009-*` sit next to the guest config.
- Phase 4 decoder done: `display_timing.hpp` decodes RPL-S (transcoders
  A–D, ports A/TC1–TC4, DPLL0–3, DP SST and HDMI) under `REIMS_TARGET_RPLS`;
  adlp unchanged; host test in `scripts/test.sh`. The iGPU is currently
  detached from the VM (back on the host's LXC).
- Done (phase 4): `display-probe/` (`ReimsDisplayProbe.kext`, approved on
  the guest; `read_probe.py`) read the IGD through passthrough without
  faults. Display engine fully unpowered (only PG0, no power wells, PLLs off,
  all DDI clocks gated); reference clock 38.4 MHz. Results table in
  docs/PORT-RPLS-UHD770.md. sudo on the guest needs the user's password.
- Phase 5 decision: port-owned display bring-up (user declined switching the
  host BIOS primary display, so no firmware/GOP route). Plan:
  docs/PHASE5-DISPLAY-BRINGUP.md. New register writes go in a separate
  component built only with `REIMS_DISPLAY_BRINGUP=1`; `kHardwareWritesPorted`
  stays false.
- Phase 5 stages 0–1 done: host reference captured
  (`~/work/igpu-notes/igpu-reference-20261009-140748/`): 4K monitor on HDMI
  TC1/PHY B via DPLL0; VBT maps HDMI-B/C → TC1/TC2, DP-D/E → TC3/TC4, level
  shifter 6. The decoder reproduces i915's mode from the real dump (now a
  test vector). Target register table in docs/PHASE5-DISPLAY-BRINGUP.md.
- Stage 2 code done, not run on hardware: `display-bringup/`
  (`core_init.hpp`, `ReimsDisplayBringup.kext`, dry run by default,
  `REIMS_BRINGUP_EXECUTE=1` for the writing build; host test in
  `scripts/test.sh`). Deviations from i915 listed in the phase 5 doc. All 55
  stage 2 target values match host i915's live state (second capture
  `igpu-reference-20261009-143055`); PHYs report 0.85 V dot0.
- Done (phase 5 stage 2 on hardware, 2026-10-09): execute build ran `OK` on
  the guest: PW1, CDCLK 307.2 MHz via pcode + PLL, DBUF, BW buddy; readbacks
  equal the host reference. PHYs/workarounds were already set by host i915
  (PHY init path not yet exercised on hardware).
- Stage 3a (code; run on hardware 2026-10-13, see the stage-3 result below):
  `display-bringup/hdmi_output.hpp` lights HDMI TC1 at 3840x2160@30 with no
  plane (black); every value checked against the third host capture
  (`igpu-reference-20261009-145010`); kext `REIMS_BRINGUP_STAGE=3`. Deviations
  (no infoframes, no VRR/IPC) in the phase 5 doc.
- Stage 3b (code; run on hardware 2026-10-13): `hdmi_scanout.hpp` puts a test
  pattern on plane 1A via a GGTT-mapped framebuffer (DBUF S2, full-range
  colour, host plane/WM/DDB values). `REIMS_BRINGUP_STAGE=3` runs 2 + 3a + 3b.
- Stage 4 handoff audit: docs/PHASE5-HANDOFF-AUDIT.md (kernel K1–K13,
  runtime R1–R8). Blocked on phase 6: the handoff needs Apple's accelerator.
- Phase 6 SKU check (port doc, "Apple TGL accelerator on RPL-S"):
  `IntelAccelerator::probe` (`0x23c1d`) accepts only vendor/device
  `0x9A408086`/`0x9A488086` (GT2, `+0x1120`=2) and panics otherwise; `a780`
  is not listed. The user lifted the earlier "do not bypass" rule on
  2026-10-13: getting the driver past this check is in scope.
- SKU bypass + GT1 L3 banks (code, not run on hardware): `REIMS_TARGET=rpls
  prepare-deferred-runtime.py` adds `je`→`jmp` at `0x23c35` (unlisted IDs
  take the GT2 store) and 2 NOPs at `0x287ea` (getGPUInfo's 1x4 GT2 branch
  sets L3BankCount=4 without bit `0x20`, the simulator flag). Checked against
  the `sle` binary: adlp output unchanged (UUID `edbb0eb4…af9a`), rpls
  candidate UUID `89dc2f52…0dcd`. Re-prepared and committed. Gate and
  DesktopLink both link the candidate as a library (`OSBundleLibraries` →
  `com.apple.driver.AppleIntelTGLGraphics`) and disagree on the version (gate
  `16.0.3`, DesktopLink `16.0.0`); the stock Apple binary omits
  `OSBundleCompatibleVersion`, so `kmutil` refused them. The candidate now
  carries `CFBundleVersion 16.0.3` + `OSBundleCompatibleVersion 16.0.0` so the
  range `[16.0.0, 16.0.3]` covers both. The reseal moves `candidate_sha256`
  to `e2999223…` (Mach-O and UUID unchanged); `deferred-runtime.json` tracks
  it.
- TGL WA table (static, 2026-10-13): `InitTglLpHwWaTable` keys only on
  revision < 4 vs >= 4, so `a780` rev `04` gets ADL-P rev `0c`'s table. Phase
  6 also needs `-allow3d` in the guest's OpenCore `boot-args`: `probe` fails
  without it for any real device ID (user's config to change).
- R1 done (code): rpls readiness is DesktopLink `RCSReady` (read-only RCS
  inspection, engine out of reset); gate and session.py check it.
- K1–K9 done (code): rpls native framebuffer takes the mode from the
  bring-up's plane 1A and BAR2; adlp objects unchanged (audit doc). Takeover
  stays behind `kHardwareWritesPorted`.
- Loaded on the guest (2026-10-13, iGPU still on the host): the deferred TGL
  candidate and the gate load on Tahoe 25G83. `kmutil showloaded`:
  `com.apple.driver.AppleIntelTGLGraphics (16.0.3)` UUID `89DC2F52…` and
  `lab.reims.ReimsADLManualActivation (0.2.2)` UUID `2DF3A522…`, the gate's
  dependency list pointing at the candidate (library linkage resolved). The
  OpenCore `Kernel > Block` on the shared identifier did **not** strip the
  candidate. `boot-args` `keepsyms=1 -allow3d`; csr `03000000` (Kext Signing
  and Filesystem Protections off). Metal bundle installed at
  `/Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle`. `ReimsADLDesktopLink`
  stays unloaded (`igpu-start` loads it). (This bullet recorded the load with
  the iGPU still on the host; the stage-3 and phase-6 bullets below are the
  later hardware window.)
- Done (phase 5 stage 3 on hardware, 2026-10-13): the stage-3 execute build
  ran `OK` on the guest (iGPU attached, from the VM snapshot). Dry run first:
  `Planned`, 0 blocked writes. Execute: `ReimsBringupResult: OK`,
  `ReimsBringupComplete: True`, 0 blocked writes, identity verified. Pipe A lit
  on HDMI TC1 at 3840x2160@30 — CDCLK PLL locked (`0x046070`=`0xc0000010`),
  DPLL0 enabled (`0x046010`=`0x8c000000`), DDI buffer enabled
  (`0x064300`=`0x80000080`), plane 1A `PLANE_SURFLIVE`=`0x01000000`, no FIFO
  underrun (`0x044408`=0), pipe A scanline counter advancing. The monitor
  shows the test pattern; its OSD reports 4K@30. Readouts in
  `~/work/igpu-notes/window-step3-{execute,gfx0}-20261013-164958.txt`. Each
  first load of a new kext binary needs one reboot (AuxKC staging), as for the
  phase-4 probe; `/private/tmp` is cleared on reboot, so re-`ditto` after.
- Phase 6 first accelerator start on hardware (2026-10-13, same window):
  `igpu-start` reached `SelfTest result=0`, `PrepareRuntime result=0` —
  **Apple's TGL accelerator probed and started on `a780` without a panic**
  (SKU bypass + GT1 L3 patch + `-allow3d` get it past `probe`/`start` on GT1).
  But its post-start GPU init then hung IOKit: the port-lit HDMI pipe went
  black and `session.py` wedged in the `PrepareRuntime` poll loop
  (`session.py:215`) because `ioreg` blocked uninterruptibly in the kernel
  (standalone `ioreg -l` and `logd` also stalled) — the accelerator deadlocked
  the registry during engine/ring/GGTT bring-up. Recovered with a reboot (not
  a snapshot rollback: disk, the compat-range fix and evidence were all fine).
  Matches NootedGreen's RPL watchdog/engine-reset reports: the bypass
  *attaches* the driver, but GPU init does not complete on RPL-S GT1. Also
  confirms the accelerator reprograms the bring-up's display (handoff can't
  assume the port-lit pipe survives `PrepareRuntime`). Evidence:
  `desktop-manual-20260917/fix/session-20261013-170332-396335-start/` +
  `~/work/igpu-notes/window-step4-*`. Details in docs/PORT-RPLS-UHD770.md
  ("First accelerator start on `a780`"). Next: narrow where it wedges
  (GuC/engine vs ring/GGTT), read-only, before another start.
- EDID over GMBUS (code; run on hardware 2026-10-13: `ReimsEDIDStatus:
  Timeout`): `display-bringup/edid_read.hpp` + test recover the real monitor
  EDID; the stage-3 execute build reads and publishes it (DDC pin 2). On
  hardware the sink did not answer DDC pin 2 during stage 3 (scanout
  unaffected). Cause (fixed in code, not yet rerun): pin 2 is the VBT value;
  on ADL-S i915 `map_ddc_pin` maps it through `adls_ddc_pin_map` to
  `GMBUS_PIN_9_TC1`. `edid_read.hpp` `adlsGmbusPin` now does the same for the
  EDID read and the SCDC setup (which had the same bug).
- Phase 6 hang diagnostics (code, not run): static read of the `sle` binary
  shows `IntelAccelerator::start` holding an IOLock + `lock_busy` across
  `startGraphicsEngine` (forcewake, `IGScheduler::initFirmware`, interrupt
  enable, contexts, three stamp submissions + `finishAllStamps`), consistent
  with the window's IOKit wedge. The profile already requests scheduler 5 and
  `MultiForceWakeSelect` 1. `REIMS_GT_SAMPLER=1` adds a read-only register
  sampler to the stage-3 bring-up kext that prints over serial (`kprintf`,
  `com.apple.kpi.libkern`); needs `qm set 113 -serial0 socket` and `serial=3`
  (docs/VM-PREP-RPLS.md 9b, docs/TEST-WINDOW-RPLS.md). Decode the host log
  with `display-bringup/read_gt_serial.py` (timeline + final-state summary);
  the runbook maps NootedGreen's RPL-P symptoms onto its output and lists
  `-disablegfxfirmware` (an Apple option present in the `sle` binary) as the
  second attempt if the sampler points at GuC firmware.
- Handoff after the blanking: options A–D in docs/HANDOFF-OPTIONS-RPLS.md
  (recommendation: fix the hang, then the native framebuffer lights the
  display itself; the next window's sampler picks whether an interim
  keep-the-order fix is worth it). The sampler logs the bring-up's first
  framebuffer GGTT PTE (`0x808000`) for that.
- Option C started (code, opt-in): `desktop-link/rpls_light.hpp` lets the
  native framebuffer light the pipe itself via the stage 2/3 sequence over its
  BAR0 and a shared black framebuffer (`display-bringup/fb_memory.hpp`).
  `configure` uses it when the plane is not lit. Gated behind
  `REIMS_RPLS_SELF_LIGHT` (off); committed rpls object byte-identical. The
  bring-up kext now shares `fb_memory.hpp` and `hdmi_scanout.hpp` refuses a
  GGTT range already in use. Host tests: test_fb_memory, updated
  test_hdmi_scanout. Not run on hardware.
- SCDC/4K60 done (code): `hdmi_scdc.hpp` sets sink scrambling + 1/40 ratio and
  `kUhd60` sets the DDI scrambling bits; stage-3 kext is 4K30 by default,
  4K60 via a `kUhd60` build. Host tests cover both. Not run on hardware.

## Working rules

- Kernel work: read-only first. Do not install or load kexts, or run
  `igpu-start`, without the user's go-ahead and a fresh VM snapshot.
- Keep each register-writing change behind `ReimsTarget::kHardwareWritesPorted`
  until the RPL-S path is ported and reviewed.
- Linux i915 (`intel_display_regs.h`, `intel_dpll_mgr.c`) is the reference for
  ADL-P vs RPL-S register differences; verify claims there, not from memory.
- Prior art (NootedGreen etc.) and the TGL binary situation are in
  docs/PORT-RPLS-UHD770.md. NootedGreen's license is non-commercial and
  GPL-incompatible: use its findings, never copy its code.
