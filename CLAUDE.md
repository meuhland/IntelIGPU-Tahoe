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
  switches `i915` ↔ `vfio-pci` with `scripts/proxmox-igpu-hookscript.sh`
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
- Next (phase 4): port `display_timing.hpp` decode for RPL-S and read the
  firmware display state read-only (needs a probe kext: go-ahead + snapshot).

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
