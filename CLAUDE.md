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
- Stage 3a code done, not run on hardware: `display-bringup/hdmi_output.hpp`
  lights HDMI TC1 at 3840x2160@30 with no plane (black); every value checked
  against the third host capture (`igpu-reference-20261009-145010`); kext
  `REIMS_BRINGUP_STAGE=3`. Deviations (no infoframes, no VRR/IPC) in the
  phase 5 doc.
- Stage 3b code done, not run on hardware: `hdmi_scanout.hpp` puts a test
  pattern on plane 1A via a GGTT-mapped framebuffer (DBUF S2, full-range
  colour, host plane/WM/DDB values). `REIMS_BRINGUP_STAGE=3` runs 2 + 3a + 3b.
- Stage 4 handoff audit: docs/PHASE5-HANDOFF-AUDIT.md (kernel K1–K13,
  runtime R1–R8). Blocked on phase 6: the handoff needs Apple's accelerator.
- Phase 6 open question (see port doc, "Apple TGL accelerator on RPL-S"):
  `IntelAccelerator::probe` maps the PCI device ID to a SKU and panics on
  unlisted IDs; `a780` is not listed. Ask upstream how ADL-P passes. Do not
  work on getting the driver past that check.
- Next: one iGPU window for all of stage 3 (dry run, then execute with a
  snapshot + go-ahead); expect the test pattern on the monitor. Stage 3a now
  sends a full-range AVI infoframe (avi_infoframe.hpp, VIC 95). Code-only
  remaining display-side: none outstanding (SCDC 4K60 done).
- EDID over GMBUS done (code): `display-bringup/edid_read.hpp` + test recover
  the real monitor EDID; stage-3 execute build reads and publishes it (DDC
  pin 2). Not yet run on hardware.
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
