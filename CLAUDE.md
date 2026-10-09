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
host display runs on a dGPU. Requirements and unknowns:
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
- KDK: no 22H730 (13.7.8) KDK was ever published; newest Ventura KDK is
  22H722 (13.7.7). Find the Ventura KDK whose AppleIntelTGLGraphics matches
  SHA-256 `ae99582bd5a945494ee684d339ac1abd0526828bcd3ea981239c0fd38f794d47`
  (22H722 has no AppleIntelTGLGraphics; earlier Ventura KDKs are unchecked,
  see docs/PORT-RPLS-UHD770.md). Expand with `pkgutil --expand-full` after
  `pkgutil --check-signature`; do not install. A different hash means the
  prepare-deferred-runtime.py offsets and ABI checks must be re-derived.
- Next: once the TGL binary is found, build
  DesktopLink with `REIMS_TGL_IMAGE`; boot with ffff isolation and record the
  guest's real IOService path for GFX0 (`ioreg -p IOService -t -w0`) to pin
  `REIMS_PCI_PATH`.

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
