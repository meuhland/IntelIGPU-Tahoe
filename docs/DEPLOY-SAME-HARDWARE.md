# Same-hardware deployment and startup

This procedure is for an environment identical to the verified machine: Intel
iGPU `8086:46a3`, revision `0c`, PCI path `GFX0`, x86_64, macOS `25G83`,
built-in 1920×1080 / 144 Hz panel. It does not automatically treat other Alder
Lake models as compatible devices, and it does not bypass the physical
identity, ABI, hash, or UUID checks in the source.

The repository does not include the Apple kernel, Metal, media, or compiler
binaries. You must obtain these components from a same-version system you are
entitled to use. The same model alone is not enough; the system build, binary
versions, and startup configuration must also match.

## 1. Pre-deployment preparation

Prepare a recovery environment you can boot into independently, and back up the
EFI, the same-named bundles in `/Library/Extensions` that will be replaced, and
the local TGL bundle in `/Library/GPUBundles`. For the first deployment, keep a
second working login or remote connection available; the script does not handle
a black screen that cannot be recovered remotely.

Verify the following conditions:

```sh
sw_vers -buildVersion
uname -m
ioreg -r -n GFX0 -l | egrep 'vendor-id|device-id|revision-id'
```

The expected system is `25G83`, the architecture `x86_64`, and the physical
hardware `8086:46a3/rev0c`. At the startup stage this approach injects
`device-id = <ffff0000>` into `PciRoot(0x0)/Pci(0x2,0x0)` so the graphics
runtime stays isolated; after boot, ManualActivation re-checks the real PCI
identity and only then adds a matching rule for the current session.

OpenCore must keep two `Kernel/Block` entries:

- `com.apple.driver.AppleIntelTGLGraphics`, x86_64, Darwin 25,
  `Strategy=Exclude`;
- `lab.reims.ReimsADLDesktopLink`, x86_64, Darwin 25, `Strategy=Exclude`.

The backlight component must be present before the framebuffer first opens.
Put the built `ReimsADLBacklight.kext` in `EFI/OC/Kexts` and add an enabled
`Kernel/Add` entry: BundlePath `ReimsADLBacklight.kext`, ExecutablePath
`Contents/MacOS/ReimsADLBacklight`, PlistPath `Contents/Info.plist`, Arch
`x86_64`, with the kernel range limited to Darwin 25. After changes, you must
check the configuration with the `ocvalidate` that matches the current
OpenCore.

## 2. External Apple components

The kernel runtime input must be a local TGL kext containing
`Contents/MacOS/AppleIntelTGLGraphics`. The verified input SHA-256 is:

```text
ae99582bd5a945494ee684d339ac1abd0526828bcd3ea981239c0fd38f794d47
```

The Metal bundle needs the same version of the Apple TGL user-space files and
their relative dependencies. The three verified key files are:

```text
AppleIntelTGLGraphicsMTLDriver  4c161bc54a3038c1e7539545175f5c928ee2fe50bda12bf833d7057337f1fecf
NativeHEVCVA                    035958cda66e3f326093a3e797f93fc3b5a27ff0f681c401c035fbdaee307761
libigdmd.dylib                  48c510a346000393e11c1dba42c48b477ec2ec43e8c833cd73f2ae48593f1edd
```

Also keep the version-matched GraphicsShared compiler bundle, the VideoToolbox
VA driver, and the VAME bundle. Mixing same-named files from another macOS
build into the bundle is out of the supported scope. The public source can only
build the compatible factory and the HEVC service library; it cannot rebuild
these Apple components.

## 3. Build in dependency order

Run from the repository root. Generate the deferred-publication TGL runtime
first, because that step generates the UUID header that ManualActivation needs
to compile:

```sh
python3 desktop-manual-20260917/fix/prepare-deferred-runtime.py \
  /path/to/local-compatible-TGL-runtime.kext

bash desktop-manual-20260917/fix/manual-gate/build.sh
REIMS_TGL_IMAGE=/path/to/local/AppleIntelTGLGraphics \
  bash desktop-reset-recovery-20260917/source/desktop-link/build.sh
bash desktop-reset-recovery-20260917/source/backlight/build.sh
bash video-decode-candidate/manual-runtime-discovery-20260917/build.sh
bash scripts/build-userspace.sh
python3 hevc-encode-implementation/backend/error-recovery-20260918/build.py
bash scripts/test.sh
```

Each kext build script signs its local output and writes the actual binary
SHA-256, the Info.plist SHA-256, and the Mach-O UUID into an adjacent
`*-current.json`. `session.py` verifies the installed bundle and loaded version
against these local receipts. Do not remove the checks, and do not mix receipts
generated on another machine with this machine's build.

## 4. Place the kernel components

The target names below are fixed names used by the run scripts. Back up any
existing target individually first, then copy this build's output:

```sh
sudo ditto desktop-manual-20260917/fix/ReimsTGLManualRuntime.kext \
  /Library/Extensions/ReimsTGLBoot.kext
sudo ditto desktop-manual-20260917/fix/manual-gate/build/ReimsADLManualActivation.kext \
  /Library/Extensions/ReimsADLManualActivation.kext
sudo ditto desktop-reset-recovery-20260917/source/desktop-link/build/ReimsADLDesktopLink.kext \
  /Library/Extensions/ReimsADLDesktopLink.kext

sudo chown -R root:wheel \
  /Library/Extensions/ReimsTGLBoot.kext \
  /Library/Extensions/ReimsADLManualActivation.kext \
  /Library/Extensions/ReimsADLDesktopLink.kext
sudo chmod -R go-w \
  /Library/Extensions/ReimsTGLBoot.kext \
  /Library/Extensions/ReimsADLManualActivation.kext \
  /Library/Extensions/ReimsADLDesktopLink.kext
sudo kmutil install --update-all
```

VideoDiscovery stays at the repository's `build/ReimsVideoDiscovery.kext`; after
the display is published, `session.py` verifies it from that location and
requests loading on demand. Do not install an offline-generated, review-only
kernel collection directly onto the system.

If macOS asks to approve system software under "Privacy & Security," complete
the normal system approval and reboot as the system requires. Do not manually
overwrite AuxiliaryKernelExtensions.kc.

## 5. Place the Metal and HEVC user-space components

First build `/Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle` from a
locally, legally obtained, version-matched TGL Metal bundle. It must keep at
least AppleIntelTGLGraphicsMTLDriver, NativeHEVCVA, libigdmd, the resource
files, and their relative dependencies. The Info.plist bundle executable name,
`CFBundleExecutable=ReimsTahoeMetalDevice`, and
`NSPrincipalClass=ReimsTahoeMetalDevice` must correspond to
`MetalPluginName=ReimsTahoeTGLGraphicsMTLDriver` and
`MetalPluginClassName=ReimsTahoeMetalDevice` in `manual-gate/profiles.plist`.

Put the three files built from the public source into that bundle:

```sh
sudo install -m 755 build/ReimsTahoeMetalDevice \
  /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle/Contents/MacOS/ReimsTahoeMetalDevice
sudo install -m 755 \
  hevc-encode-implementation/backend/error-recovery-20260918/build/libReimsHEVCService.dylib \
  /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle/Contents/MacOS/libReimsHEVCService.dylib
sudo install -m 755 build/libReimsMapResolve.dylib \
  /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle/Contents/MacOS/libReimsMapResolve.dylib
sudo codesign --force --deep --sign - \
  /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle
sudo codesign --verify --deep --strict \
  /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle
```

Do this before the graphics session starts. The HEVC service library is loaded
by the Metal factory only inside `VTEncoderXPCService`; there is no need to
inject a dynamic library into Jianying or a test client.

The map-composite library is loaded only by `FollowUpUI`'s factory. When
updating an existing install, back up the complete Metal bundle, place the
matching new factory and map library in a copy, re-sign and verify it, then
replace it all at once. The retriggered popup loads the new version only after
the old account-popup process exits; a fresh desktop takeover is not needed for
this. See [Map black-box fix and verification](../mapkit-resolve-compat/README.md).

## 6. Checks after the first reboot

After boot, confirm the isolation state first; do not perform the display
commit yet:

```sh
sw_vers -buildVersion
kmutil showloaded | egrep 'AppleIntelTGLGraphics|ReimsADLManualActivation|ReimsADLDesktopLink'
ioreg -r -n GFX0 -l | egrep 'device-id|revision-id'
ioreg -r -c IntelAccelerator -l
```

Expect the GFX0 property to still be the `ffff` startup identity; the
deferred-publication TGL runtime and ManualActivation identities to match the
local receipts; and no published IntelAccelerator yet before the first
takeover. If a version does not match, do not start; if an accelerator already
exists, the single entry point verifies its state and skips completed stages.

## 7. Manual startup

Run only the single entry point at the repository root:

```sh
sudo ./igpu-start
```

The entry point first pre-checks the physical identity and all local build
receipts, then, based on the IORegistry state, automatically completes the
hidden initialization, the RCS/reset preparation, the display commit, and any
VideoDiscovery stage not yet done. If the display is already published but the
video stage failed on system approval, it handles only the approval issue and
reruns the same command; the state machine does not re-prepare or re-commit the
display.

Save your work before running it. When the native framebuffer is at power 0
with no completed flips and the old WindowServer identity is stable, the entry
point ends the old desktop session once; if the normal exit has not completed
after 15 seconds, it first saves the call stack, then re-checks the same
process and the display state, and force-terminates the original process at
most once. The worker process detaches from the terminal session and writes its
output to startup.log in the evidence directory. A persistent marker prevents a
repeat exit or forced termination; only the 120-second timeout recorded by the
old version, within the same boot cycle and for the same original process, may
resume the forced-exit step. Do not delete the marker or retry in a loop.

It waits at most 120 seconds, and must observe a new WindowServer, the native
framebuffer at power 2, and the actual completed-flip count increasing before
it writes a successful result.json; a session already flipping normally is not
exited, and a display that is active but stalled only reports an error and
keeps the evidence. The picture still needs human confirmation; a return of 0
does not mean performance or long-term stability has been accepted. Current
known boundary: on-hardware picture recovery has been confirmed by the user,
but that run's background process query hit a sysmond service error and the
automatic-acceptance record did not complete; see CURRENT-STATE.md.

After every reboot it still stays in manual takeover; rerun the same
`igpu-start`. The project installs no automatic-takeover-at-boot task.

## 8. Readiness verification

Verify at least:

```sh
ioreg -r -c IntelAccelerator -l
ioreg -r -c ReimsIntelADLFramebuffer -l
ioreg -r -c ReimsVideoDiscovery -l
kmutil showloaded | egrep 'AppleIntelTGLGraphics|ReimsADL|ReimsVideoDiscovery'
system_profiler SPDisplaysDataType
```

The complete readiness condition is: exactly one published IntelAccelerator,
one DesktopLink, and one native framebuffer; VideoDiscovery's
`PhysicalIdentityVerified` and `Published` are true; the internal display is
still 1920×1080 / 144 Hz; real flip submission and completion keep increasing
with no long-term pending; Metal applications can create pipelines and complete
real GPU work; and the H.264 verification sample outputs correct pixels. The
appearance of a capability property does not mean pixels, performance, or
stability have passed.

Performance close to the original machine also requires the same
windows/material and test actions. The desktop's roughly 119–127 fps is a
historical result for a specific dragging scenario, not a startup-completion
criterion, and not a frame-rate guarantee for every application.

## 9. Failure and recovery

When the preparation stage fails, the display is not committed and the original
firmware desktop should still be usable. When video fails after the display is
published, handle the approval or the specific error and rerun the same entry
point; the state machine only fills in the video part. When a display freeze,
GPU reset, or WindowServer watchdog occurs, save the scene first; do not repeat
the takeover in a loop.

To recover, restore the pre-deployment `/Library/Extensions`, the Metal bundle,
and the EFI configuration, run a normal `kmutil install --update-all`, then
reboot so the loaded kernel components exit. The recovery copy in the EFI must
be prepared before any change; this repository will not guess the user's disk
identifier or overwrite a complete OpenCore configuration for them.
