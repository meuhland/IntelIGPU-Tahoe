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
