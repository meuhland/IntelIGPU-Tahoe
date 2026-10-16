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
REIMS_TARGET=rpls python3 desktop-manual-20260917/fix/prepare-deferred-runtime.py \
  ~/work/tgl-candidates/sle/AppleIntelTGLGraphics.kext
```

Without `REIMS_TARGET` the build is the unchanged ADL-P upstream target.
`REIMS_PCI_PATH` overrides the exact IOService path the activation gate accepts.

## What the rpls target changes

| Component | rpls behaviour |
| --- | --- |
| ManualActivation gate | Accepts `a780` rev `04` at `REIMS_PCI_PATH`; profiles match `revision-id` `04` |
| DesktopLink | Read-only probes (RPS, BCS, RCS capture) allowed; native framebuffer takeover refused. No ADL-P RCS PSMI workaround: `PrepareRCSADLP` runs a read-only RCS inspection and publishes `RCSReady` (reads succeed, engine out of reset), which the gate and `session.py` require instead of `RCSADLPWorkaroundLive` |
| intel_framebuffer | `configure()` refuses until the display path is ported |
| VideoDiscovery | Accepts `a780` rev `04` |
| Backlight kext | Not built; DesktopLink does not attach backlight |

`igpu-start` can therefore reach the prepared state on this target but stops
at the display commit, because the native framebuffer takeover stays refused
until it is ported and `kHardwareWritesPorted` is reviewed.

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
3. Phase 5: the port brings the display up itself (host BIOS primary
   display stays on the dGPU). Plan: [PHASE5-DISPLAY-BRINGUP.md](PHASE5-DISPLAY-BRINGUP.md).
3. Port takeover: remove DPT handling, generalize 1920×1080 constants.
4. GT1 (32 EU) topology and workarounds in the native TGL runtime. L3 bank
   count and SKU check bypass: code done (rpls runtime patches, below); GT1
   workarounds still open.

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

## Apple TGL accelerator on RPL-S (phase 6, static read of the `sle` binary)

Observed in `IntelAccelerator` (read-only disassembly, 2026-10-09):

- **Topology comes from fuses.** `getGPUInfo` reads `0x9138` (slices),
  `0x913c` (dual-subslice enable, ×2 = subslices), `0x9134` (EU disable,
  EUs/subslice = 8 − disabled) and `0x9140` (VD/VE boxes), the same registers
  Linux uses. RPL-S GT1 should read as 1 slice, 4 subslices, 32 EUs.
- **GT1-sized configuration exists.** For the TGL-LP SKU the check accepts
  1 slice / 12 subslices (L3 banks 8) and 1 slice / 4 subslices when bit 5 of
  the accelerator flags at `+0x1190` is set (L3 banks 4); otherwise it logs
  "Invalid NumSlice or NumSubSlices for TGLLP GT2" and leaves the L3 bank
  count unset.
- **SKU from the PCI device ID.** `probe` reads the vendor/device ID from PCI
  config space (not the FFFF registry property; config space shows the real
  `a780`, as the phase 4 probe confirmed) and maps a short list of Tiger Lake
  IDs to a SKU; an unlisted ID ends in a panic ("Failed to map Device ID … to
  GPU SKU", `tgl/IntelAccelerator.cpp:1141`). Neither `a780` nor ADL-P's
  `46a3` is in that list in the `sle` binary.

Added 2026-10-13 (disassembly of the `sle` binary, `llvm-objdump`):

- **The SKU list.** At `0x23c1d` `probe` loads vendor | device << 16 and
  compares it with `0x9A408086` and `0x9A488086` (both store GT tier 2 at
  `+0x1120`), plus two sentinels: `0xDEAFBEEF` → tier 1 and `0xFF208086` →
  tier 2. Anything else calls `probe.cold.1` (the panic). NootedGreen gets
  past it with a byte patch that turns the last compare into an unconditional
  jump to the tier-2 store, plus a PCI config-read hook that returns the
  injected `device-id` (`9A49`). This port patches the same compare
  instead: with `REIMS_TARGET=rpls`, `prepare-deferred-runtime.py` turns the
  `je` at `0x23c35` (`74 25`) into `jmp` (`eb 25`), so an unlisted ID takes
  the tier-2 store and `ffff` isolation stays as it is. `0xA7808086` is below
  `0xDEAFBEEE` as a signed int, so the sentinel `jg` before it is not taken.
  Tier 2 matters: `getGPUInfo` handles tier 1 differently (always 8 L3
  banks). The other device-ID compares (`probe+0x107`, `setInSimulator`) only
  special-case the sentinels. The real ID (`+0x1118`) and revision `04` still
  reach the TGL-LP workaround table; how it treats them is a phase 6 unknown.
- **What bit `0x20` of `+0x1190` is.** `IntelAccelerator::setInSimulator`
  sets it when the device ID is one of the sentinels (`0xDEAFBEEF`,
  `0xFF208086`): it is the simulator flag. `IGGuC::canLoadFirmware`,
  `loadBinary`, `dmaHostToGuC`, `checkForProgress`, the GuC work queue and CT
  buffer init and `IGAccelEventMachine::eventTimeout` test it. Setting it to
  get 4 L3 banks would put the driver in simulator mode.
- **GT1 L3 patch.** With `REIMS_TARGET=rpls`, `prepare-deferred-runtime.py`
  replaces the flag test's `je +0x12` at `0x287ea` (`74 12`) with two NOPs, so
  Apple's own 1×4 branch stores L3BankCount=4. The 1×12 branch comes first, so
  ADL-P never reaches it; the adlp candidate stays byte-identical (UUID
  `edbb0eb4…af9a`). The script asserts the symbol address, 29 bytes of context
  and the absence of relocations at the site. From the `sle` input the rpls
  candidate (both rpls patches) gets UUID `89dc2f5292cda68ec64560e2a5fa0dcd`.
- **Who reads L3BankCount.** In this kext only `getGPUInfo`'s own log calls
  read `+0x1164` directly. `IGAccelDevice::get_hw_info` copies
  `+0x1110..+0x1187` (15 qwords: revision, device ID, GT tier, topology,
  L3BankCount) into the user client's `IntelHwInfoRec`, followed by the
  timestamp frequency (`+0x1188`), the WA table (`+0xf2e`, 0xbf bytes), the
  SKU table (`+0xfed`, 0x62 bytes) and `+0x1050` (0xc0 bytes). The consumer
  is the Metal user-space driver, which reads it long after `getGPUInfo`, so
  the patch value reaches it. User space also receives the real device ID
  `0xA7808086` and revision `04`, as upstream's ADL-P passes its real
  `0x46A38086`.

### Hardware workaround table for `a780` rev `04` (checked 2026-10-13)

- `InitTglLpHwWaTable(WA_TABLE*, SKU_FEATURE_TABLE*, deviceId, revision, x)`
  is called from `IntelAccelerator::start` and `initHardwareWorkarounds` with
  `+0x1118` (vendor | device), `+0x1110` and `+0x1114`. `probe` fills
  `+0x1110` with `extendedConfigRead8(0x08)`, the PCI revision ID.
- The function ignores the device ID and the last argument. It sets the WA
  bits from one test, `revision < 4` (mask `0x1fddfaddfec00`, the early TGL
  steppings) versus `revision >= 4` (mask `0x1f8c5fec00`), plus a constant
  term from bit 15 of the revision, which is always 0 for an 8-bit ID.
- RPL-S `a780` rev `04` therefore gets exactly the workaround table ADL-P
  `46a3` rev `0c` gets on upstream's working machine. The SKU bypass adds no
  new stepping path; what differs from ADL-P is the GT size (GT1), not the
  workaround selection.
- Other users of `+0x1110`/`+0x1118`/`+0x1114` in `IntelAccelerator`: the
  probe log line, `setInSimulator` (sentinel IDs only) and `get_hw_info`. No
  other decision reads them.
- **`-allow3d` boot arg.** For any non-sentinel device ID, `probe`
  (`0x239f7`) checks `PE_parse_boot_argn("-allow3d")` and otherwise logs
  "[IGPU] Failing probe (-allow3d is NOT set in boot-arg)!" and fails. This
  is independent of the SKU check. Upstream does not document it (its EFI
  config is not published), so its machine must set it. The guest's OpenCore
  `boot-args` needs `-allow3d` before phase 6; that is a guest config change
  for the user.

### Loading the candidate on the guest (2026-10-13)

The rpls candidate was re-prepared (both patches, UUID
`89dc2f5292cda68ec64560e2a5fa0dcd`) and loads on Tahoe 25G83 with the gate:

- **Gate/DesktopLink linkage.** Both `ReimsADLManualActivation` and
  `ReimsADLDesktopLink` declare a library dependency on
  `com.apple.driver.AppleIntelTGLGraphics` in `OSBundleLibraries`, and a kext
  only satisfies a library dependency when it declares
  `OSBundleCompatibleVersion` — which the stock Apple binary omits. The two
  dependents disagree on the version they require: the gate wants `16.0.3`,
  DesktopLink wants `16.0.0`. `prepare-deferred-runtime.py` therefore sets
  `CFBundleVersion = 16.0.3` (ceiling) and `OSBundleCompatibleVersion =
  16.0.0` (floor) so the advertised range `[16.0.0, 16.0.3]` covers both.
  (First try, compat `16.0.3`, loaded the gate but failed DesktopLink in
  `igpu-start` preflight with "could not find version compatible with
  16.0.0".) Each change reseals the ad-hoc signature (the `Info.plist` hash
  lands in the Mach-O signature slot), moving the post-`codesign`
  `candidate_sha256` (now `e2999223…`); the patched Mach-O and its `LC_UUID`
  are unchanged, so the gate's `deferred_uuid.hpp` needs no rebuild.
- **OpenCore `Kernel > Block`.** The Block on the shared identifier
  `com.apple.driver.AppleIntelTGLGraphics` did not strip the manually loaded
  candidate: after reboot `kmutil showloaded` lists it at UUID `89DC2F52…`
  (version 16.0.3) and the gate (`2DF3A522…`) with its dependency list
  pointing at it. The Block still keeps Apple's own driver from auto-matching
  the FFFF device at boot.
- **Boot state.** `boot-args` `keepsyms=1 -allow3d`; csr `03000000` (Kext
  Signing and Filesystem Protections off, the latter for the
  `/Library/GPUBundles` install). `ReimsADLDesktopLink` stays unloaded;
  `igpu-start` loads it.

### First accelerator start on `a780` (phase 6, hardware, 2026-10-13)

With the iGPU attached (from the window snapshot), stage 3 lit, and the
candidate/gate loaded, `sudo ./igpu-start` ran. `session.py` reached:

```
Detected runtime state: cold
SelfTest result=0x00000000
PrepareRuntime result=0x00000000
```

**`PrepareRuntime` returned 0 — Apple's TGL accelerator probed and started on
`a780` without a panic.** The SKU bypass (`je`→`jmp` at `0x23c35`), the GT1 L3
patch and `-allow3d` together get the driver past `IntelAccelerator::probe`
and `start` on GT1; `probe`'s "Failed to map Device ID" panic and the
`-allow3d` refusal are both avoided. This is the phase-6 gate question
answered: the driver *attaches*.

**But the accelerator's post-start init then hung the GPU/IOKit.** The HDMI
output (the port-lit 4K30 pipe) went black, and `session.py` wedged in the
`PrepareRuntime` poll loop (`session.py:215`, waiting for hidden-ready). That
loop calls `ioreg` each iteration, and `ioreg` blocked **uninterruptibly** in
the kernel — a standalone `ioreg -l` never returned either, and `logd` stopped
answering. So the accelerator took or held an IOKit lock (or faulted) during
its hardware bring-up and deadlocked the registry; `session.py` never reached
its 45 s deadline because control never returned from `ioreg`. The VM required
a reboot (not a snapshot rollback — the disk, the commit fix and the evidence
were all fine).

This matches NootedGreen's RPL-P reports (engine resets, userspace watchdog
timeouts): the SKU bypass is sufficient to *attach* the driver, but the TGL
accelerator does not complete GPU init cleanly on RPL-S GT1 — the failure is
in the post-attach engine/ring/GGTT bring-up, not the device-ID check. It also
confirms the handoff audit's open interaction: the accelerator reprograms the
display the bring-up lit (the screen blanked), so a future handoff cannot
assume the port-lit pipe survives `PrepareRuntime`.

Evidence (survives reboot): `session-20261013-170332-396335-start/`
(`startup.log`, `before.plist`) and
`~/work/igpu-notes/window-step{3,4}-*-20261013-*.txt`.

### Render topology reads zero (phase 6, hardware, 2026-10-14)

With a serial console (`serial=3`, `qm set 113 -serial0 socket`) the
accelerator's `[IGPU]` HWCAPS log was captured across `igpu-start`. It names
the root cause:

```
HWCAPS - NumSlices:0      SliceCountFuse:0x0
HWCAPS - NumSubSlices:0   DualSubSliceCountFuse:0x0
HWCAPS - MaxEUPerSubSlice:8   EUDisableFuses:0x0  ExecutionUnitCount:0
HWCAPS - ERROR: Invalid NumSlice or NumSubSlices for TGLLP GT2 ...
HWCAPS - L3BankCount: 0
```

The driver builds a 0-EU GPU and then wedges in `startGraphicsEngine`
immediately after scheduler configuration (`Scheduler Throttle Cap = 100ms` is
the last `[IGPU]` line). GuC firmware is **not** loaded — the log shows `Forced
to use Host Preemptive Scheduler` / `Will fallback to host-side scheduling` —
so `-disablegfxfirmware` is irrelevant; the hang is the first stamp submissions
/ `finishAllStamps` never completing on a GPU with no execution units (which is
why `ioreg` blocked: `finishAllStamps` holds the accelerator IOLock).

Narrowed by static read of the `sle` binary and i915 v7.2:

- **Apple reads the correct registers.** `getGPUInfo` (`0x2847e`) does one
  `SafeForceWake(true, 5)` … `SafeForceWake(false, 5)` bracket and reads
  `0x9138` (`GEN11_GT_SLICE_ENABLE`), `0x913c` (`GEN12_GT_GEOMETRY_DSS_ENABLE`
  / Gen11 `GT_SUBSLICE_DISABLE`), `0x9134` (`GEN11_EU_DISABLE`), `0x9140`
  (`GEN11_GT_VEBOX_VDBOX_DISABLE`). TGL and ADL-S/RPL-S are all Gen12 pre-Xe
  and share these offsets in i915, so "wrong offset on RPL-S" is ruled out.
- ~~**Forcewake works.** The media fuse `0x9140` reads `0xf00ff`…~~ **Wrong
  (corrected 2026-10-15):** Apple logs `~raw & 0xf00ff`, so `0xf00ff` means the
  media fuse read **0** too. All four fuses read zero: the GT-asleep signature.
  See "Correction 2026-10-15" below.
- **Display power / CDCLK / DBUF are not the lever.** Running a stage-2 execute
  bring-up first (PowerWell1 `0x045404` = `HSW_PWR_WELL_CTL2`, CDCLK, DBUF)
  before `igpu-start` left `NumSlices` at 0.

So the render-GT geometry fuse mirrors genuinely read 0 in the VM. Leading
hypothesis (superseded 2026-10-15: forcewake not in effect, not a cold GT): the cold, GOP-less, passed-through GT has not been reset / had its
fuses downloaded, and `getGPUInfo` reads the mirrors very early in `start`. On
a real Mac the GOP/firmware initialises the iGPU first; this headless
passthrough never does. The GT sampler (`gt_sampler.hpp`) did not print — its
work-loop timer starved while the accelerator's `start` thread ran — so Apple's
own verbose log carried the diagnosis. Raw log and analysis:
`~/work/igpu-notes/window-20261014-serial-hwcaps.txt`.

Next, offline: (1) a GT-reset probe (read `0x9138/3c/34`, then minimally write
`GDRST` and re-read) to test whether resetting the cold GT populates the
geometry fuses; (2) a topology-injection patch for `getGPUInfo` (force
`NumSlices=1`, `NumSubSlices=4`, `EU=32`, `L3BankCount=4` when the fuse reads
come back 0). Injection alone likely will not suffice: if the GT is truly
un-initialised it cannot execute the start-up stamps either, so the hang would
move from `getGPUInfo` to the stamp wait. GT cold-initialisation is the real
target; the handoff (phase 7) stays blocked until the accelerator stays up.

#### Offline builds for the next window (2026-10-14)

Both next steps are built and host-tested (not run on hardware):

- **Topology injection** — `prepare-deferred-runtime.py` with
  `REIMS_TARGET=rpls` now forces the two geometry fuse loads in `getGPUInfo`
  to immediates (`0x9138`→1, `0x913c`→3) so NumSlices=1, NumSubSlices=4, EU=32,
  and the 1x4 branch sets L3BankCount=4. Verified against the `sle` binary;
  new candidate UUID `3a7cfb99a8d3ec6cda1108ade098a30c`. Re-prepare and
  reinstall the candidate to use it. If the GT still cannot execute, the hang
  moves from `getGPUInfo` to the start-up stamp wait — which is itself the
  signal that cold-init (below), not topology, is the remaining blocker.
- **GT-reset probe** — `gt-reset-probe/` (`gt_reset.hpp`, read-only by
  default; `REIMS_GT_RESET_EXECUTE=1` issues `GEN6_GDRST`). It reads the
  geometry fuses raw, with forcewake, and after a full GDRST, and reports
  whether the reset populated them (`read_gt_reset.py`). Run the execute build
  only from a snapshot. If `ResetPopulatedFuses` is false, GDRST is not the
  lever and the cause is deeper — a PCI FLR, or the `vfio-pci` bind reset
  clearing the host's fuse download before the VM sees the device (worth
  checking host-side: whether host `i915` reads these fuses nonzero before the
  hookscript hands off).

Run order in a window: load the injected candidate and `igpu-start`; if it now
reaches the stamp wait instead of the `getGPUInfo` error, load the GT-reset
probe (execute) to test whether a reset revives the GT.

#### Host-side fuse check (no VM window; do it when the iGPU is on the host)

The biggest open question — are the geometry fuses genuinely unpopulated, or
does the `i915`→`vfio-pci` handoff clear them? — can be answered on the host,
with the iGPU bound to `i915` for transcoding, no macOS VM involved. If the
host sees the full GT1 topology, the fuses are populated under `i915` and the
handoff (the `vfio-pci` bind reset, or `i915` powering the GT down on unbind)
is what the VM inherits as zero — which points the fix host-side rather than
in the guest.

```sh
# i915's own computed topology (expect ~1 slice, 2 DSS, 32 EU on UHD 770 GT1):
cat /sys/kernel/debug/dri/*/i915_sseu_status
# Raw geometry fuses (intel-gpu-tools); compare with the VM's zeros:
intel_reg read 0x9138 0x913c 0x9134 0x9140
```

- Host shows a populated topology / `0x9138`=`0x1`, `0x913c` nonzero → the
  handoff clears them. Next: keep the device powered across the handoff
  (avoid D3 on `vfio-pci` bind, e.g. `disable_idle_d3=1`), or see whether the
  reset method `vfio-pci` applies on bind is what wipes the fuse download, and
  whether a plain GDRST (the probe) or a GOP/firmware init is needed in the
  guest to re-download them.
- Host also reads 0 → the fuses are never populated even under `i915`, so the
  cause is more fundamental and the in-VM GDRST probe is still the test.

Record the host readings next to the window evidence.

#### Host result 2026-10-14 (iGPU on the host, `i915` bound)

```
intel_reg: 0x9138=0x00000001  0x913c=0x00000003  0x9134=0x00000000  0x9140=0x000e00fa
i915_sseu_status: Available 1 slice / 2 DSS / 32 EU;  Enabled 0 / 0 / 0 (slice power-gated, idle)
```

Decisive:

- **The silicon fuses are fine.** `0x9138`=1 and `0x913c`=3 under `intel_reg`'s
  forcewake — exactly the values the topology injection forces, so the
  injection is confirmed against real hardware, not inferred. `i915_sseu_status`
  "Available" agrees (1 slice, 2 DSS, 32 EU).
- **So the VM's zeros are not a wrong register or a Gen12 quirk.** In the VM
  the same registers read 0 *even though Apple forcewakes* (superseded
  2026-10-15: the VM read was all-zero, i.e. forcewake was not in effect). The fuse-mirror
  download that `i915` performs when it powers the GPU up at bind has not
  happened in the guest: the `vfio-pci` handoff resets the device, and the
  headless VM has no GOP/firmware to re-run the GPU power-up, so the mirrors
  stay 0. This is the "cold, GOP-less GT" from the window, now with host proof.
- **Implication for the fix.** Topology injection makes `getGPUInfo` build the
  right GPU, but if the GT itself was never powered up/fuse-downloaded it also
  cannot execute the start-up stamps, so expect the hang to move to the stamp
  wait. The real lever is giving the guest GT the power-up that `i915`/GOP
  does: the GT-reset probe tests the cheapest form (GDRST); if that does not
  re-latch the fuses, the next options are a PCI FLR or the IGD option-ROM/GOP
  route (OVMF `IgdAssignmentDxe` + Intel GOP), which the user previously
  declined but which is exactly what powers the GPU up on a real Mac.
- (`i915_sseu_status` "Enabled" reading 0 on the host is normal — slices are
  power-gated when idle; it is not evidence of the VM problem. The media fuse
  differs host vs VM, `0x000e00fa` vs `0x000f00ff`, both populated; immaterial.)

#### Cold-GT fix options

The host result reframes the blocker: the GT is cold because nothing in the
headless guest runs the GPU power-up / fuse download that `i915` (or, on a real
Mac, the firmware GOP) does. The options, cheapest first:

1. **Topology injection (done, band-aid).** Forces `getGPUInfo` to build a
   1/4/32 GPU so it clears the "Invalid NumSlice" error. Confirmed-correct
   values, but does not power the GT up, so the hang is expected to move to the
   start-up stamp wait. Keep it regardless — it is harmless and necessary once
   the GT is up, and the next window uses it to tell whether the GT can execute.

2. **GDRST reset (built: `gt-reset-probe/`).** Write `GEN6_GDRST` in the guest
   and re-read the fuses. Cheapest in-guest lever; no firmware work. Caveat: a
   GT/engine reset may not re-trigger the cold-boot fuse download (that is
   typically latched at full power-up), so it may not re-populate. The probe
   answers this; run it from a snapshot.

3. **PCI FLR (in-guest or host-side).** A function-level reset is more likely
   than GDRST to re-run fuse download, but it also tears down BAR mappings and
   device state mid-session. Safer as a host-side experiment (FLR the device,
   then check the fuses) than from inside the running VM. Not built; needs a
   host test first to see whether it even re-latches before wiring anything.

4. **IGD GOP / OpRegion route (root-cause fix; not started).** Have the guest
   firmware initialise the iGPU at boot, as real Mac firmware does. This is the
   principled fix: macOS then runs against a powered, fuse-downloaded GPU, so
   the fuse reads, the EU count, and the GT's ability to execute are all solved
   at the source, and it lights a firmware framebuffer that the takeover can
   inherit (possibly retiring the stage 2/3 bring-up). Pieces:

   - **Intel GOP driver** — extract the Gen12 Intel GOP EFI executable (and the
     VBT) from the host firmware (e.g. UEFITool on the IFWI image); version must
     match the GPU.
   - **IgdAssignmentDxe** — an OVMF DXE driver that builds the scaffolding a
     passed-through IGD lacks (OpRegion, stolen-memory/BDSM, the ASLS config
     register) so the GOP driver can bind. Mainline OVMF has a legacy version;
     Gen11+/RPL-S needs the extended "VfioIgdPkg for Gen11+" variant. QEMU
     already exposes the host OpRegion via `x-igd-opregion` (default on) and
     documents placement in `igd-assign.txt` (IGD at `00:02.0`, which we have).
   - **Cost / caveats:** a rebuilt OVMF with the Gen11+ assignment DXE, the
     extracted GOP driver loaded, and typically the IGD presented as the VM's
     primary/VGA display — which is why this collided with the earlier
     host-BIOS-primary decision. Gen12 GOP binding in a VM is finicky and not
     guaranteed. It is a strategic pivot: the port chose port-owned display
     bring-up specifically to avoid the GOP dependency, but the cold GT makes
     GOP the cleanest route if resets do not re-latch the fuses.

Decision path: run option 2 (and option 3 host-side) first — if either
re-populates the fuses and the GT executes, the GOP route is unnecessary. If
neither does, option 4 is the real fix.

#### Host-side cold-GT experiment (no VM window)

Options 2 and 3, and *which handoff step* colds the GT, can all be answered on
the Proxmox host without booting macOS: `proxmox/coldgt_check.py` hands the
iGPU to `vfio-pci` the way a VM start does, then reads the geometry fuses
(with forcewake, as Apple does) after each step — the bind / D3 round trip, a
sysfs FLR (the reset QEMU applies when it opens the device), and a GDRST (the
i915 sequence, same as `gt-reset-probe/`). Host-tested against a simulated
device (`proxmox/test_coldgt_check.py`); not yet run on hardware. Costs a few
minutes of transcoding downtime and no snapshot; worst case is the host
reboot the hookscript already documents.

Guards: refuses any write unless the driver is `vfio-pci` (or none) and no
process holds the vfio group open (VM running); refuses a device other than
`8086:a780`; `read` alone is safe under `i915`. `--wake` sets
`power/control=on` for the run (vfio-pci runtime-suspends an idle device to
D3, where BAR reads are all-ones); `--enable-decode` turns PCI memory decode on
if it is off. Both are restored on exit.

```sh
# VM 113 stopped. Copy proxmox/coldgt_check.py to the host first.
dmesg -w                                          # second shell, keep it open
/var/lib/vz/snippets/igpu-hookscript.sh 113 pre-start   # stops /dev/dri CTs, binds vfio-pci
python3 /root/coldgt_check.py read --wake         # read-only sanity check
python3 /root/coldgt_check.py all --wake | tee /root/coldgt-$(date +%Y%m%d-%H%M%S).log
/var/lib/vz/snippets/igpu-hookscript.sh 113 post-stop   # back to i915, CTs restart
cat /sys/kernel/debug/dri/*/i915_sseu_status      # i915 must recover: 1 slice / 2 DSS / 32 EU
intel_reg read 0x9138 0x913c                      # expect 0x1 / 0x3 again
```

The script prints one `=>` line per question. Reading them:

| Verdict | Meaning | Next |
|---|---|---|
| `cold before any FLR: … D3 round trip …` | either the `i915` unbind or the vfio-pci idle D3 (vfio-pci runtime-suspends right after bind, so this is the usual case) | rerun with vfio-pci `disable_idle_d3=1` (read at module load: `options vfio-pci disable_idle_d3=1` in modprobe.d, or `modprobe -r vfio-pci; modprobe vfio-pci disable_idle_d3=1` before the hookscript); still cold → the unbind, see GDRST/FLR rows |
| `cold before any FLR: … no D3 involved` | `i915` unbind (or the bind) leaves it cold | GDRST/FLR verdicts below decide; else option 4 |
| `FLR COLDS the GT` | fuses survive the bind but QEMU's reset-on-open wipes them | disable the reset before the VM starts: `echo > /sys/bus/pci/devices/0000:00:02.0/reset_method` (empty string disables reset; `echo default >` restores, per the kernel's sysfs-bus-pci ABI doc), then a VM window to see if the guest reads 1/3 |
| `FLR REVIVES the fuses` | option 3 works | wire an FLR into the handoff |
| `PM reset (D3hot->D0) COLDS the GT` | a D3 round trip wipes the fuse download (the device has no `NoSoftRst`, so D3hot→D0 is a reset) | keep it in D0: vfio-pci `disable_idle_d3=1` on the host, and find what puts it in D3 in the guest (OVMF/macOS writing PMCSR, which vfio passes through) |
| `GDRST REVIVES the fuses` | option 2 works | the in-guest `gt-reset-probe` execute is worth its window |
| `GDRST does not re-populate` + no FLR revive | resets are not the lever | option 4 (GOP), or keep the host's warm state via the two host knobs above |

The cheapest possible outcome is the first or third row: a one-line host
change (no D3, or no reset-on-open) hands the guest the warm GT that `i915`
left, and the guest reads 1/3 without any GOP work. Then run the next VM window
with that knob set: Apple's `getGPUInfo` should read the real fuses, so the
topology injection becomes a no-op safety net.

Kernel facts above (`reset_method` empty string, `disable_idle_d3` read at
module load, sysfs BAR mmap refused for regions a driver holds under strict
iomem, `pci_pm_reset` only offered without `NoSoftRst`) checked identical at
v6.6, v6.8 (PVE 8) and v6.14 (PVE 9). The i915 register facts are hardware and
do not depend on the host kernel.

##### Host result 2026-10-15 (`coldgt_check.py all --wake`, before the `pm` step existed)

```
device: driver=vfio-pci runtime_status_before=active pm_state=D0 reset_method=flr pm command=0x0002
read       raw        slice=0x1 dss=0x3 eu_disable=0x0 media=0xe00fa  populated
forcewake  forcewake  slice=0x1 dss=0x3 eu_disable=0x0 media=0xe00fa  populated (acked)
flr        before     populated
           raw        slice=0x0 dss=0x0 eu_disable=0x0 media=0x0      (all zero)
           forcewake  slice=0x1 dss=0x3 eu_disable=0x0 media=0xe00fa  populated
gdrst      forcewake  populated  ->  after_reset populated (cleared, loops=2)
```

- **The host handoff does not cold the GT.** After the `i915` unbind and the
  `vfio-pci` bind, the fuses read populated, and they still do after an FLR
  (the reset QEMU applies on open) and after GDRST. Neither reset is the
  culprit; neither is a fix needed on the host side so far.
- **The post-FLR raw read is all zero, media included** — the GT is asleep,
  and forcewake reads the real values. (This first looked different from the
  VM; it is the same — see the correction below.)
- **Not exercised: D3.** `runtime_status_before=active` — the device never
  runtime-suspended (PCI devices default to `power/control=on`, so vfio-pci's
  idle D3 only happens if something sets `auto`). Its `reset_method` includes
  `pm`, which the kernel offers only when PMCSR `NoSoftRst` is clear, so a
  D3hot→D0 transition *is* an internal reset on this device. That is the one
  reset path left, and it can be triggered from inside the guest: vfio passes
  guest PMCSR writes through, so OVMF or macOS putting the unclaimed (FFFF)
  device into D3 would reset it with nobody to re-init it.

Follow-up the same day, `coldgt_check.py pm --wake`: D3hot→D0 (the kernel's
`pm` reset) leaves the fuses populated too, even on the raw read. **No host
reset — FLR, PM, GDRST — wipes the fuse download.**

##### Correction 2026-10-15: the VM read was all zero — forcewake, not a cold GT

A static re-read of `getGPUInfo` in the `sle` binary (`llvm-objdump`, LC_UUID
`ba3aa1c0…`) overturns the "forcewake works" inference the cold-GT theory
rested on:

- **The media value was never raw.** `0x284f7: mov r14d,[rax+0x9140]; not
  r14d; mov r13d,r14d; and r13d,0xf00ff` — the logged `VEDBOXEnableFuses` is
  `~raw & 0xf00ff`; VEBox = popcount of its bits 8+ and VDBox = popcount of the
  low byte of `~raw`. The window's `0xf00ff` / VEBox 4 / VDBox 8 means
  `raw & 0xf00ff == 0`. The real register (`0xe00fa` on the host) would log
  `0x10005`, VEBox 1, VDBox 2. So in the VM **all four fuse registers read 0**
  — exactly the host's post-FLR *asleep* read, which forcewake fixes.
- **One domain.** i915's Gen12 forcewake table puts `0x9000–0x947f` (all four
  fuses) under `FORCEWAKE_GT`; an all-zero read is that domain not being awake.
- **Apple's forcewake path.** `SafeForceWake(true, 5)`: Apple's domain bits are
  1 = RENDER (`0xa278`, ack `0xd84`), 2 = MEDIA, 4 = GLOBAL (`0xa188`, ack
  `0x130044`), so 5 is render + GT — the right domains. With
  `MultiForceWakeSelect` 1 (fFlags bit 11, set by our gate profile) it keeps
  per-domain refcounts at `this+0x1290/0x1294/0x1298` and touches hardware only
  on a 0→1 transition, through `SafeForceWakeMultithreaded` with requester bit
  1; missing acks log `Retrigger … domain wake` / `SafeForceWake failed on …
  domain`. Without bit 11 it calls a pointer the framebuffer controller
  supplies (`this->0xde8`, `+8`). It silently does nothing only if fFlags bit
  12 is set, which `populateAccelConfig` does only in simulator mode (bit 5,
  `setInSimulator`: config dword `0xdeafbeef`/`0xff208086`) — not us
  (`this+0x1118` is config dword 0 = `0xa7808086`).

So the leading hypothesis flips: **the GT was asleep when Apple read it**,
because Apple's forcewake did not take effect — a refcount already above 0
(hardware never written), acks that failed (would be logged), or a GT that
cannot wake in the guest. Not a missing fuse download, so the GOP route is not
indicated by this evidence. Topology injection still clears the
`getGPUInfo` error, but if forcewake is not effective there it is likely not
effective anywhere, and the stamps would hang the same way.

Next, cheapest first:

1. **No downtime — grep the saved window log** on the guest:
   `grep -n -i "forcewake\|retrigger\|pre-si\|simulation" ~/work/igpu-notes/window-20261014-serial-hwcaps.txt`.
   `Retrigger`/`failed` lines → acks failed; nothing → forcewake was skipped or
   never needed an ack (refcount), so the GT stayed asleep silently.
   **Result 2026-10-15:** only `Detecting Pre-Si/Si Mode ... [Si]` and
   `MultiForceWake Enabled: Using 3D Driver` (both starts in the log); no
   `Retrigger`/`failed`. `SafeForceWakeMultithreaded` writes `0xa278`/`0xa188`
   unconditionally outside simulator mode and logs only when the ack poll
   (3000 × `IOPause(100)`) misses, so either the acks arrived — GT awake by
   its own report, fuses still 0, which the host never shows — or Apple's
   per-domain refcount was already above 0 and the hardware was never written.
   Step 3 separates the two.
2. **Host, Apple's requester bit:** `coldgt_check.py flr forcewake --wake
   --fw-bit 1` — confirms bit 1 wakes this GT after the reset QEMU applies.
   **Result 2026-10-15:** with `--fw-bit 1`, post-FLR raw read all zero,
   then render and GT both ack and the forcewake read is 1/3/`0xe00fa`.
   Apple's requester bit wakes this GT fine after QEMU's reset, on the host.
##### Likely root cause (static, 2026-10-15): standalone forcewake is a stub

Following the call chain in the `sle` binary explains the silent all-zero read:

- `getGPUInfo` is called from `IntelAccelerator::configureDevice` (`0x2844a`),
  which `start` reaches through vtable slot `0x5d0` at `0x240d8` — before
  `start` evaluates fFlags bit 11 and logs `MultiForceWake Enabled` at
  `0x242e3`. Inside `getGPUInfo`, `SafeForceWake` without the refcounted path
  tail-calls `this->0xde8->+8`.
- When `registerWithFramebufferController` finds no
  `AppleIntelFramebufferController` ("Could not register with IOFB! Working
  standalone (IONDRVSupport)"), `initLocalCallbackSupport` (`0x2a87a`) fills
  `0xde8` with local callbacks, and the forcewake one, `_SafeForceWake`
  (`0x2d2a0`), is an empty function: `push rbp; pop rbp; ret`. This port has
  no `AppleIntelFramebufferController`, so Apple runs standalone.
- So the `getGPUInfo` bracket does nothing, the GT is still asleep from QEMU's
  reset (the host shows the post-FLR read is all zero until forcewake), and
  `getGPUInfo` reads 0/0/0/0 with nothing logged. On ADL-P bare metal the
  firmware leaves the GT awake, which is why the same code works there.

Confirm from the saved log (no downtime): the `HWCAPS` lines should come before
the `MultiForceWake Enabled` line, and a `Could not register with IOFB` line
should be present:
`grep -n "HWCAPS - NumSlices\|MultiForceWake\|Could not register\|Pre-Si" ~/work/igpu-notes/window-20261014-serial-hwcaps.txt`.

**Fix to test: hold forcewake ourselves.** `gt-reset-probe` has a hold build
(`REIMS_GT_FORCEWAKE_HOLD=1`): it acquires render + GT forcewake with
requester bit 2 (i915 uses bit 0, Apple bit 1; a domain stays awake while any
bit is set), reads the fuses under the hold, and keeps it until unload — no
reset. Loaded before `igpu-start`, the GT is awake when `getGPUInfo` reads, so
it should see the real topology (1 slice, 4 subslices in Apple's counting, 32 EU) and the topology injection becomes a no-op
safety net. Apple's later forcewake goes through the MultiForceWake hardware
path, so this is likely only needed for start. A permanent fix would fold the
hold into the gate or DesktopLink (or patch `SafeForceWake` to take the
hardware path), once the hold build proves it on hardware.

**Permanent-fix candidate, prepared (opt-in, untested):**
`REIMS_TARGET=rpls REIMS_RPLS_FORCEWAKE_PATCH=1 prepare-deferred-runtime.py`
adds one edit: `SafeForceWake` at `0x28c5d`, `jb` → `jmp` (`72 31` → `eb 31`),
so every forcewake, including `getGPUInfo`'s early one, takes the refcounted
hardware path instead of the empty standalone callback. Safe that early: its
spinlock (`+0x1468`) is allocated in `start` at `0x23f63`, before
`configureDevice`, and the MMIO map (`+0x1240`) is set before `getGPUInfo`.
Candidate UUID `b2d3495428c12c2cb29103a87d2c63da` (computed offline; the
default rpls candidate stays `3a7cfb99…`, adlp is unaffected). Switching
means re-prepare, reinstall, and reboot; the gate does not compile
`deferred_uuid.hpp` (`session.py` checks the UUID through
`deferred-runtime.json`, per the 2026-10-15 re-prepare). The hold window
proved the diagnosis, so this is now the clean replacement for the hold
probe's forcewake, but it does not address the hang below.

##### GT watch for the hang (offline, 2026-10-15)

The hold window left the start hang with no register view: the bring-up's
sampler needs stage 3's BAR mapping (it likely never armed on 10-14, when
stage 3 refused at the GGTT guard; "work loop starved" was a guess) and
stage 3 was skipped on 10-15. Statically, the last log line comes from
`IGScheduler5::initPriorityConfig` (it creates four timers and returns), then
the per-engine `IGHardwareCommandStreamer5`s are created and `start` reaches
`startGraphicsEngine`, which submits three stamp commands and calls
`IGAccelEventMachine::finishAllStamps`. That forwards to its superclass in
`IOAcceleratorFamily2` (not in this binary), so whether it waits by polling
or on an interrupt cannot be read here.

The hold build now carries a GT watch (`gt-reset-probe/gt_watch.hpp`): two
kernel threads, not an IOKit work loop. A sampler reads the `gt_sampler.hpp`
registers every 250 ms; a reporter that never touches MMIO prints them over
serial and, if the sampler stops advancing for 2 s, prints a stall line naming
the register it is stuck reading (an MMIO read that never completes) or
`starved` (the thread is not being scheduled). The register list gained the
interrupt path (i915 v6.6): `GEN11_GFX_MSTR_IRQ` `0x190010`,
`GEN11_GT_INTR_DW0/1` `0x190018/1c`, `GEN11_RENDER_COPY_INTR_ENABLE`
`0x190030`, RCS `IMR`/`EIR`/`ESR` `0x20a8/b0/b8` and `HWS_PGA` `0x2080`.
`read_gt_serial.py` decodes all of it and summarises the hang. Host-tested;
the kext variants syntax-check against the SDK; the probe now links
`com.apple.kpi.mach` (thread and time calls, per xnu `Mach.exports`).

Reading the hang from the capture:

| Watch shows during the hang | Meaning |
|---|---|
| stall: `reading <reg>` | an MMIO read never completes; the GT/uncore is wedged at the bus level, and Apple's thread is likely stuck the same way |
| stall: `starved` | the sampler is not scheduled: a thread spinning with preemption disabled (check the VM's vCPU count) |
| samples continue, RCS ring drained (head = tail), GT interrupt pending (`gtIntrDw0` set, not cleared) | the GPU executed the stamps and raised its interrupt, which nothing serviced: interrupt delivery (standalone "Local Filter Interrupt Source") is the blocker |
| samples continue, RCS has queued work and `ACTHD` is frozen | the engine is stuck executing: GPU-side (context, ring or memory setup) |
| samples continue, RCS ring never valid / tail never moves | nothing was submitted: the wait is earlier, CPU-side |
| CSB has unread entries | execlist events raised but not processed (same interrupt question) |

##### Standalone interrupt path (static, 2026-10-15)

What "Using Local Filter Interrupt Source" sets up, from the `sle` binary:

- `IGInterruptBridge::initInterruptBridge` (`0x49f90`) logs that line when
  the bridge has no framebuffer service to register with, and calls
  `createFilterInterruptEventSource` (`0x4a13c`). That walks the provider's
  interrupt indices with `getInterruptType` and takes the first with
  `kIOInterruptTypePCIMessaged` (bit 16), falling back to index 0 only if
  there is none. The guest inventory has index 0 INTx and index 1 MSI, so
  **Apple uses MSI, index 1**, through IOPCIFamily, and adds the
  `IOFilterInterruptEventSource` to its work loop.
- `IGInterruptBridge::enable` (`0x4a332`, called from `startGraphicsEngine`
  after `initFirmware`), standalone branch: enables that event source, clears
  the master enable, runs `enableInterrupts`, then sets bit 31 of
  `GEN11_GFX_MSTR_IRQ` (`0x190010`).
- `enableInterrupts` (`0x4a42e`) writes `GEN11_RENDER_COPY_INTR_ENABLE`
  (`0x190030`) = `0x1100110` for scheduler type 1, else `0x100010`, unmasks
  the engine masks (`0x1900a8` and the others written 0), and clears pending
  identities through the `0x190070` selector / `0x190060` identity pairs. Bits
  (i915 `intel_gt_regs.h`): 4 = `GT_RENDER_PIPECTL_NOTIFY_INTERRUPT`, 8 =
  `GT_CONTEXT_SWITCH_INTERRUPT`, RCS in the high half, BCS in the low half.
  `schedulerType` maps scheduler values 3/4/5 to 2/2/1, and the forced
  fallback stores 5 (`orq $0x2800000` into fFlags bits 23–25 at `0x27b00`,
  right before "Forced to use %s Scheduler"), so the host preemptive
  scheduler is type 1 and gets context-switch interrupts as well.
- The filter (`interruptFilterHandler`, `0x4af16`) masks the master bit,
  `readAndClearInterrupts` reads and writes back `GT_INTR_DW0/1`, and
  re-enables the master.

So on paper the standalone interrupt setup is complete. What is unknown is
whether the MSI is delivered in the VM. That is visible on the host without
any new code: vfio lists the device's MSI vectors in `/proc/interrupts`,
with counts (TEST-WINDOW "Next window: watch the hang"). Combined with the
watch: an MSI vector whose count rises while `GT_INTR_DW0` keeps returning to
0 means interrupts arrive and are serviced (look elsewhere). A pending
`GT_INTR_DW0` with no MSI vector, or a count that never moves, means
delivery is the blocker.

3. **Guest, `gt-reset-probe` before `igpu-start`:** its forcewake (bit 0)
   read. 1/3 in the guest → the VM can wake the GT and the difference is
   Apple's path; 0 even with acks → the GT cannot wake in the guest.

#### Verified against i915 v6.6 (offline, 2026-10-14)

Confirms the fuses are genuinely unpopulated rather than read wrong, and
grounds both next steps (`gh api … intel_sseu.c`, `intel_gt_regs.h`,
`intel_reset.c`):

- **Gen12 slice-enable should read `0x1`.** `gen12_sseu_info_init` reads
  `GEN11_GT_SLICE_ENABLE & GEN11_GT_S_ENA_MASK` and does
  `drm_WARN_ON(s_en != 0x1)`. So Apple's `SliceCountFuse:0x0` is not a Gen12
  quirk — on real silicon that bit is set. The mirror really is empty.
- **Subslices come from an enable mask.** Gen12 subslice/DSS count is
  `hweight(GEN12_GT_GEOMETRY_DSS_ENABLE)` (`0x913c`), expected nonzero (a full
  TGL GT2 reads `0x3f` = 6 DSS; a UHD 770 GT1 should read a 2-DSS mask).
  Apple's `DualSubSliceCountFuse:0x0` is the key missing datum.
- **EU uses a disable mask.** `~GEN11_EU_DISABLE` (`0x9134`); `0x0` disabled =
  all present, so Apple's `EUDisableFuses:0x0` is ambiguous (also what an
  empty read gives). (The media `0xf00ff` is Apple's `~raw & 0xf00ff`, i.e. a
  raw 0 — see the 2026-10-15 correction; no fuse data was live.)
- **Injection target is i915's own model.** i915 hardcodes 1 slice for Gen12
  (`intel_sseu_set_info(sseu, 1, 6, 16)`) and derives the rest from the DSS
  mask, so forcing `NumSlices=1` + a UHD 770 GT1 DSS population (2 DSS → 4 SS
  in Apple's traditional-SS counting, 8 EU/SS = 32 EU, L3=4) is principled,
  not a guess.
- **GDRST sequence for the probe.** `GEN6_GDRST` = `0x941c`;
  `GEN6_GRDOM_FULL` = bit 0, `GEN6_GRDOM_RENDER` = bit 1. i915
  `gen6_hw_domain_reset` writes the mask then polls `0x941c` until the mask
  bits self-clear (2 ms timeout), repeated twice on pre-MTL Gen12. GDRST is
  not in the GT power well and needs no forcewake for the write/read — so the
  probe can reset without first solving forcewake.

So: topology injection is well-founded and worth having (if it reaches the
stamp wait, that itself tells us the GT believes it has EUs), but the reset
probe tests the root cause — whether a cold GT populates its geometry fuses
once reset. The reset probe must be run from a snapshot: a GDRST on the
passed-through GT is the first time this port writes `0x941c`, and the host
must still recover the IGD for QuickSync afterward (stop VM → hookscript
rebinds `i915`; reboot the host if it does not).

### Forcewake hold on hardware (phase 6, 2026-10-15)

Candidate `3a7cfb99` (SKU + L3 + geometry injection) loaded, then the
forcewake-hold probe (`REIMS_GT_FORCEWAKE_HOLD=1`, UUID `0C82A034`) before
`igpu-start`, serial console capturing.

- Probe: raw fuses `0x9138/913c/9134/9140` = 0/0/0/0; held (render + GT
  forcewake, both acked) = `1`/`3`/`0`/`0xe00fa`, identical to the host's i915
  reads. The GT wakes in the guest; the empty standalone `SafeForceWake` was
  why the fuses read zero.
- `igpu-start` with the hold: HWCAPS now reports the real topology from the
  fuses (`SliceCountFuse 0x1`, `DualSubSliceCountFuse 0x3`, NumSlices 1,
  NumSubSlices 4, EU 32, L3BankCount 4, media `0x10005` = 1 VEBox / 2 VDBox;
  no "Invalid NumSlice" error).
- The hang is unchanged: the last `[IGPU]` line is still `Scheduler Throttle
  Cap = 100ms.`, `session.py`'s `ioreg` child sits in state `U`, and nothing
  more appears on serial over 3-4 minutes (no reset, timeout or watchdog
  panic). The zero topology was a real bug but not the cause of the hang.
- Open suspect from the log: `Failed to register with service. Using Local
  Filter Interrupt Source!` (standalone mode, no framebuffer controller).
- Interrupt inventory of the IGD (read-only, same day): index 0 INTx level
  (INTA, line 22), index 1 MSI edge (cap `0xac`, not enabled), so the
  fallback source has an interrupt to attach to. Delivery during `start` is
  untested. Evidence: `~/work/igpu-notes/window-20261015-intr-probe.txt`.

Evidence: `~/work/igpu-notes/window-20261015-gt-hold.txt`,
`desktop-manual-20260917/fix/session-20261015-130724-068558-start/`.

### Host MSI check during the hang (2026-10-15 evening)

Hold+watch probe `833D0664` loaded (held fuses 1/3/0/`0xe00fa`, watch armed),
candidate `3a7cfb99`, `igpu-start`: the same silent hang after `Scheduler
Throttle Cap = 100ms.` On the Proxmox host:

| | `/proc/interrupts` for `00:02.0` | `lspci` MSI |
|---|---|---|
| before `igpu-start` | `vfio-intx` (IRQ 16), count 0 | `Enable-` |
| during the hang, two reads 5 s apart | `vfio-msi[0]` (IRQ 168), count 0 on every CPU both times | `Enable+`, address `fee00018` |

Apple's standalone interrupt path therefore does enable the IGD's MSI and vfio
sets up the vector, but the GPU raises no interrupt at all while `start`
waits in `finishAllStamps`. Either the start-up stamps never execute, or the
GT does not generate their interrupt (master IRQ or engine masks). The GT
watch meant to tell these apart produced no `ReimsGT` output on serial in the
window capture, although the guest published `ReimsGTWatch: True`; that is
open. Evidence: `~/work/igpu-notes/window-20261016-hold-watch.txt`.

**Why the watch printed nothing (offline, 2026-10-15).** It printed with
`kprintf`, and on a release kernel `kprintf` only reaches the serial port when
the `debug` boot-arg has `DB_KPRT` (`0x8`). Without it `disable_serial_output`
stays true and `kprintf` hands the text to `os_log` alone (xnu
`pexpert/i386/pe_kprintf.c` `PE_init_kprintf` and `kprintf`;
`osfmk/kern/debug.h` `DB_KPRT 0x8`), and `os_log` is exactly what blocks
during the hang. Apple's `[IGPU]` lines reach the capture because Apple also
calls `IOLog`, which writes to the console unless
`disable_iolog_serial_output` is set (default false; `iokit/Kernel/IOLib.cpp`
`_IOLogv`), and `serial=3` puts the console on the serial port. The bring-up
sampler had the same flaw, so "the sampler never printed" on 2026-10-14 was
likely this too. Fixed: the watch and the bring-up sampler now print with
`IOLog`. The threads themselves were probably running (`ReimsGTWatch: True`
means both started); they printed into the unified log only.

**Reading the next capture.** `read_gt_serial.py <capture> --msi flat` now
ends with a hang verdict for RCS that answers the three questions directly:
did `ACTHD` move, is anything pending in `GT_INTR_DW0`, is bit 31 of
`GEN11_GFX_MSTR_IRQ` set. It then checks the RCS half of
`GEN11_RENDER_COPY_INTR_ENABLE` and RCS `RING_IMR`, and, with the measured flat
MSI count, picks one of:

- no context on RCS (ring not valid) → never submitted (CPU/submission side);
- work queued and `ACTHD` never moved → never executed (GPU side: context,
  ring or memory setup);
- executed, `GT_INTR_DW0` pending, master clear → masked at the master;
- executed, pending, master set, MSI flat → raised in the GT but not
  reaching the MSI;
- executed, nothing pending, completion bit not enabled or masked in IMR →
  masked at the engine;
- executed, enabled, nothing pending, MSI flat → the stamps did not request
  an interrupt.

### GT watch on hardware: never executed, RCS catastrophic error (2026-10-15 late)

The IOLog watch printed (129 samples, `vm113-samples-20261015-144420.txt`,
watch armed at 0.5 s, `igpu-start` at about 418.6 s, capture to 567.7 s).
Every line was cut at 255 characters, keeping the first 26 of 33 columns:
IOLog's console path holds one line in `SERIAL_CONS_BUF_SIZE` (256) and drops
the rest unless `drain_uart_sync=1` (xnu `osfmk/console/serial_general.c`
`console_printbuf_putc`). The lost columns were `rcsEir`, `rcsEsr`,
`rcsHwsPga` and the display/PTE ones. Fixed: samples and the header now print
as parts of 10 columns (`ReimsGT <seq> <ms> @<first> …`, under 160
characters), and `read_gt_serial.py` reassembles them and still reads
truncated one-line captures.

Decoded (`read_gt_serial.py … --msi flat`):

| | before `igpu-start` | from `igpu-start` (same 250 ms sample) |
|---|---|---|
| `GEN11_GFX_MSTR_IRQ` | `0` | `0x80000000` (master enabled), unchanged after |
| `RENDER_COPY_INTR_ENABLE` / RCS `IMR` | `0` / `0` | `0x01100110` / `0` (completion + context-switch enabled, nothing masked) |
| `GT_INTR_DW0` / `DW1` | `0` | `0` throughout: nothing ever pending |
| RCS `HEAD`/`TAIL`/`START`/`CTL`, `ACTHD` | `0` | `0` throughout: no context ever loaded, `ACTHD` never moved |
| RCS context-status pointer | `0x7007` | `0x0`, never advances: no context-switch event |
| RCS `EXECLIST_STATUS_LO` | `0x1` | `0x40003018` |
| RCS `MI_MODE` | `0x200` (idle) | `0x0` (not idle, not stopped) |
| RCS `RING_RESET_CTL` | `0` | `0x4` = **`CAT_ERROR`**; bit 0 (`REQUEST_RESET`) pulses every ~8.25 s, `READY_TO_RESET` never |
| `ERROR_GEN6` | `0x37` | `0x7b` |
| `GEN12_RING_FAULT_REG` | `0` | `0` |
| forcewake acks GT / Render | `0x4`/`0x4` (hold bit 2) | `0x2`/`0x2`: the hold's bit 2 was cleared; GT ack dropped to 0 for ~8 s once |

Answers: `ACTHD` never moved, nothing is pending in `GT_INTR_DW0`, and the
master bit is set (with the engine enable on and `IMR` clear). So this is
**never executed, not an interrupt mask**, consistent with the flat MSI
count. The interrupt path is armed; the GPU never ran anything.

New: in the very sample where `start` armed interrupts, RCS reported a
**catastrophic error** (`RESET_CTL_CAT_ERROR`) with no context ever loaded.
Something then requests an engine reset every ~8 s using the normal
handshake and waits for `READY_TO_RESET`. i915 notes that for `CAT_ERROR`
the ready-to-reset handshake must be bypassed (`intel_reset.c`,
HAS#396813: write `CAT_ERROR`, let the hardware clear it), so that recovery
can never complete. That fits a silent, endless wait in `finishAllStamps`.

So the question becomes why the first submission to RCS raised a
catastrophic error in the VM. Leading candidates (to be checked against the
next capture's error columns):

- the GPU fetching the context image, ring or status page at an address the
  host IOMMU does not map for this VM (a GGTT entry pointing outside guest
  RAM, e.g. at stolen memory the VM does not really have). The host logs
  such DMA faults (`DMAR:` lines naming `00:02.0` and the address), a
  no-code check;
- a context image or engine setup the hardware rejects (Apple built it for
  TGL GT2; a GT1 or RPL-S difference), which `EIR`/`ESR`, `IPEIR`,
  `INSTDONE` and `EXECLIST_STATUS_HI` should narrow down.

The sampler now also reads RCS `IPEIR`, `INSTDONE`, `DMA_FADD`, `BBADDR`,
`EXECLIST_STATUS_HI`, `CONTEXT_CONTROL` and `GEN12_FAULT_TLB_DATA0/1`
(i915 v6.6), and the verdict reports `CAT_ERROR`, the reset requests and the
context-status pointer.

#### Host DMAR fault and where the GPU writes (2026-10-15)

The host logged one IOMMU fault from the IGD:
`DMAR: [DMA Write NO_PASID] Request device [00:02.0] fault addr 0x0 [fault reason 0x02] Present bit in context entry is clear`.
Reason `0x02` means the device had **no** IOMMU context at that moment, i.e.
it was bound to `vfio-pci` but not attached to the VM's domain (before QEMU
opened it or after QEMU released it). While the VM runs, the context entry is
present and a page-table problem would log reason `0x05`/`0x06` instead. So
this fault says the GPU was still issuing **writes to address 0** when it was
detached; it does not by itself show a bad mapping during the hang. Inside the
VM a write to 0 would not fault at all; it would land in guest RAM at physical
0.

Placed in time (host `dmesg -T`, 2026-10-15): the fault is at **14:53:14**,
the only DMAR line; i915 rebound at 14:58:18 and loaded cleanly (GuC
70.49.4, HuC authenticated, display up), and the next VM start at 15:18:05
logged two vfio resets. The watch capture's last sample is about 567 s after
it armed (~14:44), with `igpu-start` at +418 s (~14:51), so the fault lines up
with stopping the hung VM: QEMU released the device, its IOMMU context went
away, and the wedged GPU was still writing to address 0. That makes it likely
the GPU was writing to address 0 all along during the hang, silently into
guest RAM at 0. The hang therefore leaves the hardware recoverable by a plain
VM stop and i915 rebind; no host reboot is needed.

**Full-watch window (2026-10-16, probe `C4EA066E`, guest commit `498d37d`).**
Same verdict, now with the status-page PTE. VM 113 has 16 GiB (no balloon),
so on q35 guest RAM is about `0–0x7fffffff` plus `0x1_0000_0000` to about
`0x4_7fffffff`:

- RCS status page GGTT `0x40004000` → PTE `0x1_4b75708b`: valid, physical
  `0x14b757000`, **inside guest RAM**. The status page is not the write to 0,
  and its mapping is backed.
- GGTT entry 0 → PTE `0x4_ff44b08b`: valid, physical `0x4ff44b000` (~20 GiB),
  **outside guest RAM**: a stale host address left by i915 (like the host
  framebuffer range below GGTT `0x2a80000`). The VM's IOMMU domain does not
  map it, so a GPU access through GGTT page 0 would fault as reason
  `0x05`/`0x06` at that address; none was logged before the hang.
- Apple's own GGTT allocations start at 1 GiB (status page at `0x40004000`);
  the low GGTT, which the framebuffer controller normally owns, still holds
  host i915 leftovers.

Neither mapping explains the write to `0x0`. Next candidate: PPGTT. Apple runs
execlist contexts with PPGTT ("Graphics Address: PPGTT, Single Address
Space"); a context whose page-table root (PDP/PML4 in the context image) is 0,
or whose PPGTT entries point at 0, would make the engine walk and write
around physical 0 and could raise the catastrophic error on the first context
load. The error columns (`EIR`/`ESR`, `IPEIR`, `INSTDONE`,
`EXECLIST_STATUS_HI`, `CONTEXT_CONTROL`) and the post-stop DMAR check should
narrow that down.

**DMAR timing, second occurrence (host `dmesg -T`, 2026-10-15).** Baseline
before the kext (15:21): no DMAR line; the single vfio reset at 15:21:49 was
the guest reboot that loaded the probe. Then:

| time | host event |
|---|---|
| 15:25:50 | `DMA Write … [00:02.0] fault addr 0x0 [fault reason 0x02]` (one line) |
| ~15:28 | `qm stop` (per the guest session) |
| 15:29:17 | one vfio reset (QEMU teardown) |
| 15:31:05 | i915 rebound, GuC/HuC/display OK |

The fault is about 2–3 min **before** the stop, as on the first occurrence
(14:53:14, i915 back at 14:58:18). So it is not the teardown: some earlier
event left `00:02.0` briefly without an IOMMU context entry while the hung
GPU was writing to address 0. The likely event is the guest's reboot/shutdown
after the hang (runbook: Ctrl-C, then reboot). Open: the exact guest action at
15:25:50 and the host kernel version (recent kernels can attach a blocking
IOMMU domain around a device reset, which would log reason `0x02` for DMA in
flight). The fact that holds either way: while hung, the GPU keeps issuing
DMA **writes to address 0**, which in the attached VM land silently in guest
RAM at 0.

**Full decode of the 45-register window (2026-10-16, host
`read_gt_serial.py`; samples armed at 0.5 s, `igpu-start` at ~116.4 s, capture
to 322 s).** Beyond the verdict above:

- **Error identity is clean.** `EIR`, `ESR`, `IPEIR`, `IPEHR`, `DMA_FADD`,
  `BBADDR` stay 0 throughout: the engine never parsed a command. It failed in
  or before context load.
- **Context restore started and produced no ring.** At submission
  `CONTEXT_CONTROL` (`0x2244`) goes `0 → 0x100` while `RING_HEAD/TAIL/START/CTL`
  stay 0. In a Gen12 RCS context image `CONTEXT_CONTROL` is the first
  register restored, then ring head, tail, start and control; `RING_CTL` 0 is
  "no valid ring". So either the image's ring fields are zero or the restore
  stopped after its first entry: a bad context image, or one fetched from the
  wrong memory. `INSTDONE` drops bit 21 (`0xfffffffe → 0xffdffffe`): one unit
  left busy.
- **Apple adopts the old GGTT.** One sample before the start burst (#464,
  116.35 s) the existing PTEs change flags `…001 → …08b` with their addresses
  kept: GGTT entry 0 (host `0x4ff44b000`) and GGTT `0x01000000` (host
  `0x2f5d5c000`, stage 3 not loaded this time). `initGlobalGTT` re-encodes the
  whole table with Apple's attributes and keeps what is there, as it would
  keep a firmware framebuffer on a Mac; here that keeps host i915 addresses.
- **Status page fine**: GGTT `0x40004000` → `0x14b757000`, guest RAM.
- `RING_RESET_CTL`: `CAT_ERROR` from submission, 12 reset requests (~8.25 s
  apart) without `READY_TO_RESET`, then none after ~282 s.

Leading theory: the context image the engine loads is not the memory Apple
filled. If its LRCA's GGTT entry, or the PPGTT root restored from it, points
at physical 0, the restore reads guest RAM at 0 (mostly zeros: no valid ring,
catastrophic error) and context save or page walks write to 0, matching the
DMAR write to `0x0`. Next capture reads exactly that, using the registers
Apple's own `IGHardwareRingBuffer::dumpRegistersRCS` dumps: `RCS_ELST0/1`
(`0x2250/0x2260`, execlist element descriptors, LRCA in bits 31:12),
`RCS_PDP0_DESCRIPTOR` (`0x2270/0x2274`, PPGTT root, PML4 for 4-level),
`GFX_MODE` (`0x229c`), `STATUS_CTXT_ST_PTR` (`0x223c`), and a derived
`lrcaPte` (the GGTT PTE of the LRCA in `ELST0`). The decoder prints the
context's GGTT entry and the PPGTT root and flags physical 0.

**DMAR timing settled (host journal, 2026-10-15).** Nothing happened on the
host at 15:25:50: no Proxmox task, no reset, no hookscript; the VM ran with
the device attached until a `qmreset` at 15:29:18 (the logged `resetting`)
and the stop at 15:31:06 (`post-stop` → i915 at 15:31:07). (Kernel
`7.0.2-4-pve`: its reset-time blocking domain needs ATS and runs after bus
master is cleared, and vfio's release reset is silent, so neither explains
it.) Instead the fault lines up with **`igpu-start`**: in that capture the
submission came 116.4 s after the probe armed, and arming ~15:23:54 (after the
15:21:51 guest reboot) puts it at 15:25:50. The first window matches the same
way: submission at +418.6 s and fault at 14:53:14 (armed ~14:46:15; the
capture ran to ~14:55:42, then stop and i915 at 14:58:18). So **the GPU's
DMA write to `0x0` happens at the first context submission**, the moment of
`CAT_ERROR`, with the VM attached, and the IOMMU blocked it. That is what a
context whose LRCA maps to physical 0 would do: on Gen12 execlists the
engine's first writes for a context go to its per-process status page, page
0 of the context image. Why the IOMMU reports reason `0x02` rather than a
page-table fault while the device is attached is open; the next capture's
`lrcaPte` should show the physical address.

What would send GPU writes to 0: a valid GGTT entry whose address is 0. The
engine's hardware status page, which the GPU writes on context events, is the
prime candidate. The sampler now also reads the status page's own GGTT entry,
derived from `rcsHwsPga` (`hwsPteLo/Hi` at `0x800000 + (HWS >> 12) * 8`), and
GGTT entry 0 (`ggttPte0Lo/Hi`); the decoder prints both and flags a valid PTE
with physical address 0.

Ruled out on the way, statically: Apple's stolen-memory pool. In standalone
mode `registerWithFramebufferController` never fills `_stolenMemInfo`
(`+0x13d0`; the framebuffer controller normally does, via
`FBCLASSRegisterAccelerator`). The pool is then built with zero ranges:
`prepareDescriptorList` creates no descriptor, `IGStolenMemoryPool::init`
still returns true, and `allocate` returns NULL without a range allocator.
Nothing calls `IGStolenMemoryPool::allocate` directly. So stolen memory hands
out no address-0 objects. (`IntelSharedMemInfo` at `+0x13c0` gets only
`0x18000000` in its second dword; its meaning is open.)

#### 55-register watch window (2026-10-15 afternoon, probe `D23B0993`)

Recorded facts; interpretation left to the offline session. Hold+watch probe
`D23B0993` (55 registers, six-part samples, none cut), candidate `3a7cfb99`,
gate `2DF3A522`. Guest probe read: held fuses 1/3/0/`0xe00fa`, both acks,
`ReimsGTWatch: True`. Guest `date` right before `igpu-start`: **15:48:28**.

- Host DMAR: one new fault at **15:48:58**, `DMA Write NO_PASID ... fault addr
  0x0 [fault reason 0x02]` (30 s after the guest's `date`, clocks as-is); the
  only other one in the log is 15:25:50 from the previous window. Host
  baseline 15:46:22: `vfio-intx` 0, MSI `Enable-`. During the hang
  (15:49:25): MSI `Enable+` address `fee00018`, `vfio-msi[0]` count 0 on
  every CPU across 5 s.
- Decoder (`read_gt_serial.py` 5389adf, `--msi flat`): 47 samples, 513 ms to
  195737 ms. Before the submission all PTEs read with flags `0x001`
  (`hwsPte`/`ggttPte0`/`lrcaPte` `0xe_4b757001`, `fbPte` `0x4_bdd5c001`), CSB
  `0x7007`, `ERROR_GEN6` `0x37`. At 129396 ms those four PTEs change to flags
  `0x08b`; at 129657 ms `rcsExecStatus` `0x40003018`, CSB `0`, `RING_RESET_CTL`
  CATASTROPHIC, MI_MODE running, `ERROR_GEN6` `0x7b`, master IRQ enabled,
  `rcsBcsIntrEn` `0x01100110`, `rcsHwsPga` `0x40004000`, `rcsInstdone`
  `0xffdffffe`, `rcsCtxCtl` `0x100`, status-page PTE `0x1_3a99408b`.
  **`rcsElst0/1`, `rcsPdp0`, `rcsGfxMode` and `rcsCtxStPtr` read 0 in every
  sample.** Final GGTT entry 0 `0xe_4b75708b`; fault TLB `0x1f_ffffffff`;
  GuC `0x1` (in reset). Forcewake: the probe's bit 2 cleared at the
  submission, GT ack briefly 0 at 137940 ms, back at 145974 ms. Four reset
  requests (170.9, 179.1, 187.4, 195.5 s), READY_TO_RESET never seen.
  Verdict: NEVER EXECUTED, not an interrupt problem.

Evidence: `~/work/igpu-notes/window-20261015-decode-d23b.txt` (guest);
capture on the Proxmox host.

**Interpretation (offline, 2026-10-15) and what the binary says about the hang.**

- `RCS_ELST0/1` (`0x2250/0x2260`), `PDP0` (`0x2270`) and
  `STATUS_CTXT_ST_PTR` (`0x223c`) read 0 throughout: on Gen12 these
  Gen8-era registers do not show the submitted context. Dropped from the
  sampler.
- `GFX_MODE` (`0x229c`) = 0 is **Apple's normal state**, not a VM fault.
  `IntelAccelerator::initModeRegisters` (`0x28e1a`, from `startGraphicsEngine`
  under `SafeForceWake(true,7)`) writes `0x00080000` to each engine's mode
  register (`kIGHwCsDesc+0x39`, `0x229c` for RCS). That is i915's
  `_MASKED_BIT_DISABLE(GEN11_GFX_DISABLE_LEGACY_MODE)`, where i915 enables it.
  It is the only writer of `0x229c`, and the working ADL-P setup runs the
  same code with the same forced host scheduler (`GraphicsSchedulerSelect`
  5). The ADL-P accelerator very likely starts standalone too (DesktopLink's
  framebuffer is created at the display commit and answers `ra` with
  Unsupported), so neither difference is the VM-specific cause.
- **How Apple submits** (`IGHardwareCommandStreamer5::submitExecList`,
  `0x3a65a`): two 64-bit element descriptors (context object `+0x89`, low
  dword LRCA | flags, high dword context ID) to `kIGHwCsDesc+0x19`, then `1`
  to `+0x1d`. For RCS those are `0x2510` and `0x2550`, i915's
  `RING_EXECLIST_SQ_CONTENTS` / `RING_EXECLIST_CONTROL` (`EL_CTRL_LOAD`).
  `kIGHwCsDesc` (`0xdc5b0`, 0x79 bytes per engine) also holds `0x2234`
  execlist status, `0x23a0`/`0x2370` CSB pointer/buffer, `0x2080` status
  page, `0x229c` mode, `0x20d0` reset control, `0x20b0`/`0x20b4` EIR/EMR,
  `0x2358` timestamp, and `0xa278`/`0xd84` forcewake request/ack.
- **How Apple waits** (`IGHardwareRingBuffer::sleepForStamp`, `0x41fe4`): it
  **polls a stamp value in memory** (`this+0x30`) every 100 µs
  (`assert_wait_timeout`), with a 5 s timeout (`0x4c4b40` × 1000 ns; 500 s in
  simulator mode). Not an interrupt. On timeout the caller
  (`IGAccelFIFOChannel::resetHardwareAndReplay`) resets and replays, which is
  the ~8 s reset-request cadence: an endless timeout → reset → replay loop
  that `CAT_ERROR` never lets succeed. `PanicOnGPUHang` (property, fFlags bit
  17) makes the timeout call `PE_enter_debugger`, which acts only with the
  `debug` boot-arg's `DB_NMI` (`0x4`).
- **Apple has a full hang analysis** that this path never runs:
  `IGHardwareRingBuffer::debugGraphicsEngine` → `doHangAnalysis` +
  `dumpHangAnalysis` (`dumpExecutionListStatus`, `dumpPageTableStatus`,
  `dumpRingBuffer`, `dumpRegisters`, "Intel GPU Hang Summary"), written with
  `drvErrLog` into a 64 KiB buffer at `this+0x8a8` and then
  `signalHardwareError(eRestartRequest)`. It is reached only from
  `waitTimeout` (ring-space waits), not from the stamp wait. A possible later
  probe: call `dumpHangAnalysis` on Apple's ring buffer and print the buffer.

**Next capture, built:** the sampler reads the submit queue
(`0x2510–0x251c`, `rcsSq0Lo` etc.) and derives `lrcaPte` from it. When a new
LRCA appears, the hold build dumps, once, through the GGTT entries and
read-only mappings of guest memory: the context's page 0 (per-process
status page) and page 1 (register state, decoded as the
`MI_LOAD_REGISTER_IMM` list), the ring's first page from the restored
`RING_START`, and the 256 GGTT entries from `0x40000000` (Apple's
allocation window). The decoder prints the restore list (`CONTEXT_CONTROL`,
`RING_*`, `PDP*`), the ring's commands, and the GGTT run list with any entry
at physical 0. Host-tested; kexts syntax-checked.

#### Apple's first context, read from the binary (offline, 2026-10-16)

- **Image layout = Gen12 (i915) layout.** `IGHardwareContext` keeps the image
  in an `IGSharedMappedBuffer` (`+0x98`); page 1 is the register state. Apple
  writes `RING_TAIL` at `+0x101c` (dword 7), `RING_START` (the ring's GPU VA,
  `initRingGPUVirtualAddress`) at `+0x1024` (dword 9) and `RING_CTL` at
  `+0x102c` (dword 11) = `((RingSizeKB·1024 / 4096) − 1) << 12 | 1`, e.g.
  `0x7001` for 32 KiB: valid (`initRingControl(true)` from `initRingRegisters`).
  These are i915's `CTX_RING_*` slots.
- **Bootstrap.** New contexts are normally copied from a golden "safe image"
  (`restoreFromSafeImage`: accelerator `+0x1268`, size `kIGHwCsDesc+0x55`),
  and `startGraphicsEngine` creates that image: it runs the first context,
  waits (`finishAllStamps`), then `copyContextImage`. The **first** context
  therefore starts from `initWithOptions`: a 0x140-byte per-engine template
  (`g_cInitGfxRingContextRCS` at `0xc04a0`; also BCS/CCS/VCS/VECS/POCS)
  copied to `+0x1000`, `CONTEXT_CONTROL` forced to `0xffff0008` when fFlags
  `0x18000` is set, PDPs from the PPGTT object at `+0x109c/+0x10a4`, and the
  descriptor (`+0x89`) set to `0x309` (valid, privilege, priority 1) before the
  addressing mode (`getHardwareContextAddressMode`) and LRCA are added.
- **The RCS template**, decoded: `NOP`, `LRI(11)` (`0x2244` =
  `0xffff0000`, ring `0x2034/30/38/3c`, `0x2168/40/10`, `0x21c0/c4/c8`),
  zeros where i915 has `CCID`/`0x2b4`, `LRI(9)` (`0x23a8`, `PDP3…PDP0`),
  `LRI(1)` `0x21b0` (`POSH_LRCA`), `LRI(1)` `0x20c8` (`R_PWR_CLK_STATE` = 0),
  `LRI(2)` `0x4204/0x4200`. A sane Gen12 restore list (absolute RCS
  offsets, no CS_MMIO).
- **The observed `CONTEXT_CONTROL` does not come from this image.** Restoring
  Apple's image would leave the register at `0x0` or `0x8` (value bits of
  `0xffff0000`/`0xffff0008`). Every window read `0x100` after submission
  (bit 8, `GEN12_CTX_CTRL_OAR_CONTEXT_ENABLE`), and the ring registers stayed
  0 although Apple's image carries a valid `RING_CTL`. Together with the GPU's
  DMA write to `0x0` at submission, this points at **the engine loading
  memory other than the image Apple filled**: the LRCA's GGTT mapping, as
  seen by the GPU, leads elsewhere. The context dump (`ReimsGT ctx`) shows
  the PTE and what is actually in that memory.
- `R_PWR_CLK_STATE`: i915 programs `0x80041000` on Gen12 for one slice
  (`intel_sseu_make_rpcs`, `has_slice_pg` only); TGL GT2 and UHD 770 GT1
  are both one slice, so this is not expected to separate them. The
  template leaves it 0; the decoder checks the value in the dump.

The sampler also gains `RING_CCID` (`0x2180`, the context the engine
considers current) and the RC6/power-gating state (`RC_CONTROL` `0xa090`,
`RC_STATE` `0xa094`, `PG_ENABLE` `0xa210`, `RC6_LOCATION` `0xd40`,
`RC6_CTX_BASE` `0xd48`), 58 columns. The dump covers 96 dwords per page so it
reaches `R_PWR_CLK_STATE`. The decoder compares the restore list with
i915's `gen12_rcs_offsets`, decodes the submitted descriptor's flags, and
checks `R_PWR_CLK_STATE`.

**Control run planned:** [LINUX-CONTROL-RPLS.md](LINUX-CONTROL-RPLS.md), i915
in a VM with this exact passthrough, execlists (`enable_guc=0`) and GuC, to
tell "Apple's submission" from "this VM setup".

#### Context dump window (2026-10-16 morning, probe `A76EC7C7`)

Recorded facts; interpretation left to the offline session. Hold+watch probe
`A76EC7C7` (52-register watch plus the one-shot dump at first submission),
candidate `3a7cfb99`, gate `2DF3A522`; VM 113 8 cores, 1 socket. Guest probe
read: held fuses 1/3/0/`0xe00fa`, both acks, `ReimsGTWatch: True`; serial
parts complete. The guest `date` before `igpu-start` was not recorded.

- Host DMAR: one write-to-0 fault (reason `0x02`) at **10:12:50**; none in
  the baseline (10:10:49; VM start 10:06:08, guest reboot 10:08:27). During
  the hang MSI `Enable+` (`fee00018`), `vfio-msi[0]` count 0 across 5 s.
- Decoder (`--msi flat`): 62 samples, 40697 ms to 251036 ms. Same sequence as
  the 2026-10-15 windows: at 91459 ms the four PTEs go from flags `0x001` to
  `0x08b`; at 91715 ms CATASTROPHIC, `ERROR_GEN6` `0x7b`, `EXECLIST_STATUS`
  `0x40003018`, `rcsHwsPga` `0x40004000` (PTE `0x1_359ff08b`). Submit queue:
  `SQ0` `0x20_402b7319`, `SQ1` `0x40_402d7319`. `CCID` `0x80`, `GFX_MODE` 0,
  `RC_CONTROL` 0, `RC_STATE` `0x00040000`, `PG_ENABLE` `0x4`, `RC6_LOCATION`
  `0x1`, `RC6_CTX_BASE` `0x807f7001`. Twelve reset requests, ~8.3 s apart
  (124.6 s to 215.9 s), READY_TO_RESET never seen. Verdict: NEVER EXECUTED.
- Context dump, LRCA GGTT `0x402b7000` (descriptor: valid, privilege, legacy
  64b, priority 1, sw ctx id 1, class 0): page 0 PTE `0x1_3575908b`, 96 dwords
  all zero. Page 1 PTE `0x1_3575808b`, restore list: `CONTEXT_CONTROL`
  `0xffff0011`, `RING_HEAD` 0, `RING_TAIL` `0x58`, `RING_START` `0x402cb000`,
  `RING_CTL` `0x7001`, BB/indirect-ctx/timestamp 0, `PDP1-3` 0, `PDP0`
  `0x1_34f94000`, `POSH_LRCA` `0x402b3000`, `R_PWR_CLK_STATE` `0x80000000`,
  `0x4204`=0, `0x4200`=`0x40008000`. Decoder flags: the list differs from
  i915's Gen12 RCS layout at entry 11 (`0x3a8, 0x28c, 0x288, 0x284` vs i915
  `0x180, 0x2b4, 0x3a8, 0x28c`); `R_PWR_CLK_STATE` `0x80000000` vs i915
  `0x80041000` for 1 slice. Ring page (PTE `0x1_3834208b`): `MI_NOOP` x3,
  three `PIPE_CONTROL`, `MI_0x07`, `MI_NOOP` x3, two `PIPE_CONTROL`,
  `MI_0x07`, then `MI_NOOP`. GGTT window `0x40000000`-`0x400fffff`: 256 of 256
  entries valid, none at physical 0 among the 24 runs pasted (232 more runs in
  the host decode).

Evidence: `~/work/igpu-notes/window-20261016-decode-a76e.txt` (guest);
capture and full decode on the Proxmox host.

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

## Rejected approaches

### Sharing the iGPU between the host and the VM (vGPU / SR-IOV)

The guest needs the whole real iGPU because Apple's TGL driver drives the PF
directly (FFFF isolation, native MMIO/GGTT, the stage-3 display writes), so
the host cannot keep QuickSync on the same device while the VM runs.

- **GVT-g** (Intel mediated `mdev` vGPU, the closest analogue to NVIDIA vGPU)
  supported only Broadwell–Comet Lake and was removed before Gen12; RPL-S is
  not supported.
- **SR-IOV**: the `a780` PF does advertise the capability (`lspci -vvs
  00:02.0`: `Capabilities: [320 v1] SR-IOV`, currently `Enable-`), but it does
  not help here. Enabling it keeps the PF bound to the host driver (the PF is
  what provisions VFs), which is exactly what the VM cannot share; a VF is a
  PF/GuC-mediated surface with no display engine (Intel GPU SR-IOV is
  render/media only), so Apple's driver — which needs the real PF and a
  display — cannot use one.

The way to run longer bring-up sessions without starving transcoding is
separate silicon: a cheap discrete GPU on the host for QuickSync, leaving the
iGPU passed through to the VM.

### A paravirtual Metal driver for the macOS guest

Idea: instead of passing the iGPU through, have a macOS guest Metal driver
marshal commands to a host renderer, so the host keeps the GPU. Not viable for
this project:

- **No public Metal driver SPI.** Apple's GPU bundles implement private,
  undocumented driver protocols (the `MTLDevice`/command-queue backend, the
  IOGPU resource interfaces). There is no supported way to write a conforming
  Metal backend; it would mean reverse-engineering that private ABI, which
  also shifts across macOS releases. This is the same reason the port reuses
  Apple's TGL Metal bundle rather than writing its own, and why
  `pawan295/Appleinteltgldriver.kext` has no Metal.
- **Both ends plus translation.** There is no macOS virtio-gpu guest driver to
  build on, so it would mean writing the guest Metal backend, a guest→host
  transport, a host service translating Apple Metal command encoding to a host
  API (e.g. Vulkan via Mesa), and shader translation from Apple AIR to SPIR-V
  — a multi-person-year effort against moving, undocumented targets.
- **It discards the premise.** The project's value is native hardware Metal
  through Apple's real driver; a paravirtual stack inherits none of the
  bring-up work and would give translated Metal at worse performance and
  compatibility than passthrough.

It is not impossible in the abstract — Apple ships exactly this as *Apple
Paravirtualized Graphics* for macOS guests under Virtualization.framework on
Apple Silicon hosts — but that is Apple-internal, Apple-Silicon-host, and
closed; nothing equivalent exists for a QEMU/Linux host with an Intel GPU, and
Hackintosh VMs use passthrough for acceleration. A paravirtual *framebuffer*
(no Metal, like the current QXL display) remains the only para-virtual option,
and gives display without acceleration.
