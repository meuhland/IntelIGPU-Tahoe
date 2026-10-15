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
- **Forcewake works.** The media fuse `0x9140` reads `0xf00ff` (VEBox 4 / VDBox
  8) inside the same forcewake bracket where the geometry fuses read 0.
- **Display power / CDCLK / DBUF are not the lever.** Running a stage-2 execute
  bring-up first (PowerWell1 `0x045404` = `HSW_PWR_WELL_CTL2`, CDCLK, DBUF)
  before `igpu-start` left `NumSlices` at 0.

So the render-GT geometry fuse mirrors genuinely read 0 in the VM. Leading
hypothesis: the cold, GOP-less, passed-through GT has not been reset / had its
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
  the same registers read 0 *even though Apple forcewakes*. The fuse-mirror
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
- **Two different zero states.** Right after the FLR the raw read is zero for
  every fuse *including media* — the GT is asleep, and forcewake reads the
  real values. The VM's reading was different: slice/DSS 0 but media
  `0xf00ff` (real value `0xe00fa`). The GT answered, but its fuse mirrors held
  something other than the download. `coldgt_check.py` now tags these
  separately (`asleep` vs `cold`, the VM signature).
- **Not exercised: D3.** `runtime_status_before=active` — the device never
  runtime-suspended (PCI devices default to `power/control=on`, so vfio-pci's
  idle D3 only happens if something sets `auto`). Its `reset_method` includes
  `pm`, which the kernel offers only when PMCSR `NoSoftRst` is clear, so a
  D3hot→D0 transition *is* an internal reset on this device. That is the one
  reset path left, and it can be triggered from inside the guest: vfio passes
  guest PMCSR writes through, so OVMF or macOS putting the unclaimed (FFFF)
  device into D3 would reset it with nobody to re-init it.

Next, both cheap:

1. **Host, `coldgt_check.py pm --wake`** (new step; included in `all`): forces a
   D3hot→D0 through the kernel's `pm` reset method (restores `reset_method`
   afterwards) and compares forcewake reads. `COLDS` with the `cold` signature
   reproduces the VM's state on the host and names the trigger.
2. **Guest, `gt-reset-probe` early in the boot.** Load the execute build
   before stage 3 / the gate and read raw, forcewake and post-GDRST values
   (the host just showed GDRST is benign). Cold at that point → the GT is cold
   when macOS first sees it (QEMU/OVMF/guest PM); populated → something this
   port loads later colds it, so repeat after stage 3 to bisect. Also check
   the guest's PM state: `ioreg -r -n GFX0 -l | grep -i 'IOPowerManagement\|CurrentPowerState'`.

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
  empty read gives) — only the media fuse `0x9140=0xf00ff` proves any fuse
  data is live.
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
