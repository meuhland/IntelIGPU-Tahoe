# Port: Raptor Lake-S UHD 770 (8086:a780 rev 04)

Work in progress on branch `uhd770-port`. Upstream targets the ADL-P laptop
`8086:46a3` rev `0c`; this port targets the desktop UHD 770 passed through to a
macOS Tahoe `25G83` guest on Proxmox (QEMU/KVM).

## Selecting the target

Every kernel identity check reads `common/reims_target.hpp`. Kernel build
scripts source `scripts/target-flags.sh`:

```sh
REIMS_TARGET=rpls bash desktop-manual-20260917/fix/manual-gate/build.sh
REIMS_TARGET=rpls REIMS_TGL_IMAGE=/path/to/AppleIntelTGLGraphics \
  bash desktop-reset-recovery-20260917/source/desktop-link/build.sh
REIMS_TARGET=rpls bash video-decode-candidate/manual-runtime-discovery-20260917/build.sh
```

Without `REIMS_TARGET` the build is the unchanged ADL-P upstream target.
`REIMS_PCI_PATH` overrides the exact IOService path the activation gate accepts.

## What the rpls target changes

| Component | rpls behaviour |
| --- | --- |
| ManualActivation gate | Accepts `a780` rev `04` at `REIMS_PCI_PATH`; profiles match `revision-id` `04` |
| DesktopLink | Read-only probes (RPS, BCS, RCS capture) allowed; native framebuffer takeover and the ADL-P RCS PSMI workaround refused (`RCS` result 20) |
| intel_framebuffer | `configure()` refuses until the display path is ported |
| VideoDiscovery | Accepts `a780` rev `04` |
| Backlight kext | Not built; DesktopLink does not attach backlight |

`igpu-start` therefore stops at the prepare stage on this target. That is
intended until phases 4–5 (read-only mode decode, then takeover) are ported.

## Proxmox guest setup

Verified 2026-10-09 on Proxmox VE 9.2.21, pve-qemu-kvm 11.1.1, q35, OVMF;
host board MSI (IGD subsystem `1462:7e03`).

- **Placement.** Proxmox `hostpci` puts the IGD behind a root port
  (`04:00.0` under `00:1c.0`) and cannot choose the guest address, and its
  `legacy-igd` option targets i440fx. Attach it with raw `args:` instead, and
  pin the `qemu-xhci` from `args:` elsewhere so it does not take slot 2:
  `-device qemu-xhci,bus=pcie.0,addr=0x5 ...
  -device vfio-pci,host=0000:00:02.0,bus=pcie.0,addr=0x2`
  (and no `hostpci` entry for the IGD).
- **Host ownership.** With raw `args:` Proxmox no longer rebinds the device.
  The host uses the IGD for LXC QuickSync (`i915`), so it is not bound to
  `vfio-pci` at boot. The VM hookscript `proxmox/igpu-hookscript.sh` stops
  the containers that pass through `/dev/dri`, unbinds `i915` and binds
  `vfio-pci` in `pre-start`, then returns the IGD to `i915` and restarts those
  containers in `post-stop`; any failure aborts the VM start and gives the IGD
  back. The macOS VM and LXC QuickSync cannot use the IGD at the same time.
- **Guest view.** `8086:a780` rev `04`, class `0x038000` (the host BIOS makes
  the dGPU primary), at `IOService:/AppleACPIPlatformExpert/PCI0/AppleACPIPCI/S10@2`
  with an `IONDRVFramebuffer` attached. QEMU's DSDT names it `\_SB.PCI0.S10`
  and reuses `S10` for slot 2 behind its PCI bridges, so a global rename is
  unsafe.
- **GFX0 rename.** `session.py` looks the IGD up by name (`ioreg -n GFX0`) and
  the rpls default gate path is `.../PCI0/AppleACPIPCI/GFX0@2`. OpenCore
  `ACPI > Patch`, scoped to the root-bus device:

  | Key | Value |
  |---|---|
  | `TableSignature` | `44534454` (`DSDT`) |
  | `Base` | `\_SB.PCI0.S10` |
  | `Find` | `5331305F` (`S10_`) |
  | `Replace` | `47465830` (`GFX0`) |
  | `Count` | `1` |
  | `Limit` | `16` |

  `Limit` keeps the match at the device's own name; if `Base` does not
  resolve, nothing is renamed and the gate fails closed. Verified on OpenCore
  1.0.7: `ioreg -p IOService -t -w0 | grep -E 'GFX0@2|S10@2'` shows one
  `GFX0@2` (a780 rev 04) under `PCI0/AppleACPIPCI`, and the bridge's `S10@2`
  unchanged.
- **SIP.** The port's kexts are ad-hoc signed, so SIP must allow untrusted
  kexts: `csr-active-config` = `01000000` (only `0x1`), listed under
  `NVRAM > Delete` so OpenCore replaces the stored value. `csrutil status`
  then reports a custom configuration with only Kext Signing disabled. The
  Metal bundle may need further relaxation in phase 6 (unknown; upstream
  documents none).
- **ffff isolation.** OpenCore `DeviceProperties > Add >
  PciRoot(0x0)/Pci(0x2,0x0) > device-id` = `ffff0000`. Verified: `GFX0`'s
  registry `device-id` reads `<ffff0000>` (what `session.py` requires), the
  `compatible` list still carries `pci8086,a780` and `pci1462,7e03`,
  `IONDRVFramebuffer` stays attached, and no Intel graphics kext loads.
- **QEMU IGD support (11.1.1, `hw/vfio/igd.c`).** `a780` is recognised as
  gen 12. At any guest address QEMU emulates the 64-bit BDSM register
  (`0xC0`) and, with `x-igd-opregion` (default on), exposes the host OpRegion
  through fw_cfg. Legacy mode is gen 6–9 on i440fx only and never applies.
  Stock OVMF does not consume those fw_cfg entries.
- **Firmware display (phase 5).** Takeover inherits the firmware mode, so the
  guest firmware must light a monitor through the IGD. Per QEMU's
  `igd-assign.txt` this needs the IGD at `00:02.0` (done), VGA class (IGD set
  primary in the host BIOS), and an option ROM built from `IgdAssignmentDxe`
  (VfioIgdPkg for Gen11+) plus the Intel GOP driver extracted from the host
  firmware. Then drop the emulated VGA (`vga: qxl` is kept for now as the
  console).
- Apple drivers may behave differently under a hypervisor; watch for it.

Snapshot the guest before every kext install. QEMU's gdbstub can debug the
guest kernel (including the TGL kext) from the host.

## Open items

1. Done: read-only display probe run on the guest (results below).
2. Done: `display_timing.hpp` RPL-S decoder (below).
3. Phase 5 prerequisite: a firmware-initialised display. Either the guest
   firmware lights the IGD (IGD primary in the host BIOS + an
   `IgdAssignmentDxe` + Intel GOP option ROM), or the port gains a full
   display bring-up (power wells, CDCLK, DPLL, PHY, link training), which
   upstream never needed.
3. Port takeover: remove DPT handling, generalize 1920×1080 constants.
4. GT1 (32 EU) topology and workarounds in the native TGL runtime.

## RPL-S display timing decode

`display_timing.hpp` keeps the ADL-P decoder verbatim for `adlp` builds
(`intel_framebuffer.o` text and strings identical) and adds an RPL-S decoder
under `REIMS_TARGET_RPLS`, checked against Linux i915 v7.2:

| | ADL-P (upstream) | RPL-S |
|---|---|---|
| Display version | 13 | 12 (ADL-S descriptor; `a780` in `INTEL_RPLS_IDS`) |
| Route | transcoder A, DDI A, eDP | transcoders A–D; ports A, TC1–TC4 (3–6) on PHYs A–E |
| Clock select | `0x164280` bits 1:0 | `0x164280` (PHY A–C) / `0x1642bc` (D–E), 2 bits at `(phy%3)*2`; clock-off bits 10/11/24/4/5 |
| PLLs | DPLL0–1 | DPLL0–3: enable `0x46010/14/18/30`, CFGCR0 `0x164284/8c/94/c0` (DPLL2 uses the DPLL4 registers) |
| Output | DP SST | DP SST, or HDMI/DVI: pixel = port × 8 / bpc, ×2 for YCbCr 4:2:0 |

Reference clock (`0x51004`), the 38.4 MHz fraction workaround (#22010492432,
display 12+), the PLL formula and `TRANS_MULT` follow i915 unchanged.
`decode(s, t, m)` decodes one transcoder; `decode(s, m)` returns the first
enabled one. `scripts/test.sh` runs `test_display_timing.cpp` (HDMI and DP
vectors over DPLL0/2/3 and PHYs A/C/D, plus failure cases).

## Read-only display probe

`display-probe/` builds `ReimsDisplayProbe.kext` (`REIMS_TARGET=rpls bash
display-probe/build.sh`). It matches on `IOResources`, finds the IGD at
`ReimsTarget::kPCIPath`, checks the real PCI identity, and refuses if PCI
memory decoding is off rather than enabling it. It maps BAR0, reads the
`ReimsDisplayTiming` registers plus power-well, DC-state, strap,
`DDI_BUF_CTL` and plane registers, decodes transcoders A–D, and publishes
`ReimsDisplayProbeRegistersV1` (offset/value pairs) and
`ReimsDisplayProbeTranscoders` on its own service. The binary imports only
PCI config reads (no config or I/O writes) and stores nothing through the
mapping. `kmutil print-diagnostics` resolves its dependencies.

Run (iGPU attached, fresh snapshot, user go-ahead):

    sudo kmutil load -p display-probe/build/ReimsDisplayProbe.kext
    python3 display-probe/read_probe.py
    sudo kmutil unload -b lab.reims.ReimsDisplayProbe

A first load of a new kext needs approval in System Settings → Privacy &
Security and a reboot; then load again. `kmutil` also wants the bundle
owned by `root:wheel`, so stage a copy first:
`sudo ditto …/ReimsDisplayProbe.kext /private/tmp/ReimsDisplayProbe.kext &&
sudo chown -R root:wheel /private/tmp/ReimsDisplayProbe.kext`.

### Result 2026-10-09 (VM 113, IGD at `00:02.0`, OVMF without IGD ROM)

Loaded and read without faults: identity verified, `PCICommand` `0x7`, no
all-ones reads. The display engine is unpowered and unconfigured:

| Register | Value | Meaning |
|---|---|---|
| `DSSM` `0x51004` | `0x40000020` | reference clock 38.4 MHz (fraction WA applies) |
| `FUSE_STATUS` `0x42000` | `0x88000000` | fuses loaded; only PG0 distributed |
| `PWR_WELL_CTL1/2` | `0` / `0` | no BIOS or driver power-well requests |
| `DC_STATE_EN` | `0` | no DC states |
| `DPCLKA_CFGCR0/1` | `0x01e07c00` / `0x00000030` | clock-off set for PHYs A–E |
| `DPLL0–3_ENABLE` | `0` | all PLLs off (CFGCR hold identical stale values) |
| `DDI_BUF_CTL`, `TRANS_*`, `TRANSCONF`, `PLANE_CTL_1` | `0` | no port, pipe or plane enabled |

All four transcoders decode as `Disabled`. Nothing initialised the display:
OVMF had no IGD option ROM and the host's `i915` left it powered down on
unbind. Raw dump: `~/work/igpu-notes/display-probe-20261009.{txt,plist}`.

## Apple TGL binary source

Upstream pins `AppleIntelTGLGraphics` 16.0.0 by SHA-256 (`ae99582b…7d47`) and
names Ventura 13.7.8 (22H730). No KDK was published for 22H730, and the newest
Ventura KDK, 13.7.7 (22H722), contains no `AppleIntelTGLGraphics` (checked
2026-10-09). Earlier Ventura KDKs are unchecked.

The TGL kexts and GPU bundles circulating in the community live in folders
named `sle_Internal` (with debug framebuffer builds) in the repositories below.
They look like Apple-internal builds rather than public releases: provenance
and redistribution rights are unverified, and a hash match only proves it is
the file upstream used. Never commit these binaries to this repository.

Checked 2026-10-09 (NootedGreen `95f8189`; macintelk/drm has the same blobs,
and neither repo's history has another version):

| Copy | Bundle identifier | SHA-256 |
|---|---|---|
| `sle_Internal/sle` | `com.apple.driver.AppleIntelTGLGraphics` | `890735a9…27ac` |
| `sle_Internal/le` | `com.xxxxx.driver.AppleIntelTGLGraphics` | `1b2f5aa3…15a2` |

Neither matches upstream's hash. Both are 1,825,024 bytes with LC_UUID
`ba3aa1c0fe6b33b39d8573f848394e3d`. They differ only in the signing
identifier inside the code signature (10 bytes), so they are one build. Both
pass every check in `prepare-deferred-runtime.py` (`IntelAccelerator::start`
at `0x23ecc`, vtable `+0x5b0` → `registerService`, call bytes at `0x246b5`)
and in `verify_tgl_map_abi.py`. Upstream's input is most likely the same build
with a different signature; that is inferred, not proven.

The port uses `sle`: its identifier matches the gate profiles, and
`prepare-deferred-runtime.py` accepts its hash next to upstream's. Prepared
from it (Info.plist blob `dc62aaa9`, same NootedGreen commit): candidate
SHA-256 `3c3def0b…1c0c`, UUID `edbb0eb480dfe8e487eca25a0221af9a`. The
candidate differs from `sle` only in the 6-byte `registerService` NOP at
`0x246b5`, the LC_UUID, and the re-signed code signature (`__LINKEDIT` and
signature sizes).

The Metal user-space files from the same `sle` folder (checked 2026-10-09)
also miss upstream's three SHA-256 pins (docs/DEPLOY-SAME-HARDWARE.md):

| File | SHA-256 | LC_UUID | Evidence of same build |
|---|---|---|---|
| `AppleIntelTGLGraphicsMTLDriver` | `e21a8d12…7d3f` | `2B849E57…3585` | Strong: equals `tglUUID` pinned in every `metal_entry_loader.m`; the fixed method offsets in `texture_sync_compat.m` (`0x57cb3`, `0x57ccb`) and `resolve_compat.m` (`0x7f496`, `0x4a8cf`) land on the expected methods |
| MTLDriver `libigdmd.dylib` | `830cb227…94c6` | `1FB19E6C…3C9D` | None: upstream pins no UUID or offset |
| GLDriver `libigdmd.dylib` | `c6401834…42a6` | `1FB19E6C…3C9D` | Same build as above; differs only in its install name (raw build-system path) |
| `AppleIntelTGLGraphicsVADriver` (assumed `NativeHEVCVA`) | `d265f203…0129` | `2B208B8F…6926` | None: the name mapping is inferred from `service_bridge.c` |

All are thin x86_64, built against SDK 10.16, and signed under Apple
identifiers with no team identifier. Use the MTLDriver copy of `libigdmd`.
The VA driver and `libigdmd` stay unverified until the HEVC and Metal paths
run on hardware.

## Prior art

Reviewed read-only on 2026-10-09; nothing downloaded or run.

### sgiammori/NootedGreen

Lilu plugin that spoofs newer Intel iGPUs as Tiger Lake (`9A49`) so Apple's
TGL framebuffer and accelerator kexts load; lists `0xA780` (UHD 770). Its
README reports booting to login on Raptor Lake under macOS Sonoma 14.7.1
(23H222), with Metal still unstable. It patches the driver at runtime, unlike
this project's fixed patch plus companion kexts, but hits the same driver on
the same hardware family. Findings it reports, relevant to phases 5–6:

- **Scheduler/firmware:** GuC firmware initialisation fails on RPL/ADL; it
  disables firmware loading and selects the host preemptive scheduler (type 5).
  Upstream's runtime profile already sets `GraphicsSchedulerSelect=5` and
  `SchedulerFallbackOnFirmwareFail=1`.
- **Engine reset:** Apple's `resetGraphicsEngine` times out (returns 1025) on
  RPL, the driver retries until a `userspace watchdog timeout` panic. Its
  mitigation reports success when the RCS ring looks idle. Upstream's rule is
  never to fake GPU completion, so treat this as a symptom to expect, not a fix.
- **Context submission:** remaining blocker in its README: the execlist stalls
  and the ring-context page of the logical context is not populated on RPL, so
  submitted contexts never become active.
- **DBUF:** per-plane display buffer allocation is programmed by the
  accelerator kext, not the framebuffer kext; with the framebuffer alone,
  `DBUF_BUF_CFG` stays zero and output is fragmented. For phase 5, check DBUF
  state of the inherited firmware mode before taking over the plane.
- **Display ownership:** rewriting plane registers while WindowServer
  composites caused watchdog panics, consistent with upstream's single-owner
  handoff.
- **DMC:** it can load the ADL-P display microcontroller firmware on RPL.

License: "Thou Shalt Not Profit" (non-commercial), incompatible with this
repository's GPL-3.0. Use it for findings only; do not copy its code.

### macintelk/drm

NootedBlue-based fork with extracted Linux i915/xe sources for browsing,
decompiled output of the TGL kexts, and the same `sle_Internal` binaries. No
license. Its `le_kexts.sh` copies dropped folders onto `/` as root and deletes
`/Library/KernelCollections/AuxiliaryKernelExtensions.kc`; do not run it.

### pawan295/Appleinteltgldriver.kext

From-scratch Tiger Lake driver (`8086:9A49`) that reuses Apple's bundle
identifier `com.apple.driver.AppleIntelTGLGraphics` but contains no Apple code.
Its README states it has no command submission or Metal yet. Not a substitute
for the Apple binary this project patches.
