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

## Proxmox guest requirements (unverified)

- Host: IOMMU enabled, `8086:a780` bound to `vfio-pci`, host display on the dGPU.
- Guest IGD at `00:02.0` on the root bus. The OpenCore isolation property
  targets `PciRoot(0x0)/Pci(0x2,0x0)` and the gate expects `...@2`. Proxmox's
  `hostpci` may place the device behind a bridge; raw `args:` may be needed.
- SSDT naming the device `GFX0` (so `session.py`'s `ioreg -n GFX0` works).
  Default path assumed: `IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/GFX0@2`.
  Confirm with the guest's `ioreg -p IOService -t -w0` and set
  `REIMS_PCI_PATH` if it differs. A mismatch fails closed.
- Display takeover inherits the firmware mode, so OVMF must light the monitor
  through the IGD: this needs an IGD OpRegion and a Raptor Lake GOP ROM.
  Use no emulated VGA, so the IGD framebuffer is the guest's only display.
- Gen12 stolen memory (BDSM) passthrough depends on QEMU's IGD quirk support
  for Gen11+; check the Proxmox QEMU version.
- Apple drivers may behave differently under a hypervisor; watch for it.

Snapshot the guest before every kext install. QEMU's gdbstub can debug the
guest kernel (including the TGL kext) from the host.

## Open items

1. Boot the guest with `ffff` isolation; record the IOService path and the
   firmware display state (DDI, transcoder, DPLL) read-only.
2. Port `display_timing.hpp` decode for the RPL-S clock/PLL registers.
3. Port takeover: remove DPT handling, generalize 1920×1080 constants.
4. GT1 (32 EU) topology and workarounds in the native TGL runtime.

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
`prepare-deferred-runtime.py` accepts its hash next to upstream's. Running
that script with it regenerates `manual-gate/deferred_uuid.hpp` and
`deferred-runtime.json`; a scratch run gave candidate UUID
`edbb0eb480dfe8e487eca25a0221af9a`. The Metal user-space files are in the same
`sle` folder and are not yet hash-checked against upstream's pins.

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
