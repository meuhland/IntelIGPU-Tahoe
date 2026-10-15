# Preparing the VM for an RPL-S test window

What to build and install on the macOS guest (VM 113, Tahoe 25G83) before
attaching the iGPU. Everything here runs with the iGPU still on the host; the
window itself is [TEST-WINDOW-RPLS.md](TEST-WINDOW-RPLS.md).

Run from the repository root on the guest. Paths assume the TGL source kext at
`~/work/tgl-candidates/sle/AppleIntelTGLGraphics.kext` (see CLAUDE.md).

## 1. Update the checkout

```sh
git pull origin uhd770-port
```

Needs `e94d4a9` or later.

## 2. Re-prepare the TGL candidate (SKU bypass + GT1 L3 patches)

The script refuses to overwrite an existing candidate, so move the old one
aside first:

```sh
mv ~/work/tgl-candidates/ReimsTGLManualRuntime.kext ~/work/tgl-candidates/ReimsTGLManualRuntime-6byte.kext
REIMS_TARGET=rpls python3 desktop-manual-20260917/fix/prepare-deferred-runtime.py \
  ~/work/tgl-candidates/sle/AppleIntelTGLGraphics.kext
mv desktop-manual-20260917/fix/ReimsTGLManualRuntime.kext ~/work/tgl-candidates/
```

Expect `"candidate_uuid": "3a7cfb99a8d3ec6cda1108ade098a30c"` (the SKU + L3 +
geometry-fuse-injection build; `89dc2f52…` was the earlier SKU+L3-only
candidate without the injection). Any other UUID or an assertion error: stop.

## 3. Build the kernel components for rpls

The gate compiles against the `deferred_uuid.hpp` step 2 just regenerated.

```sh
REIMS_TARGET=rpls bash desktop-manual-20260917/fix/manual-gate/build.sh
REIMS_TARGET=rpls REIMS_TGL_IMAGE=~/work/tgl-candidates/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
  bash desktop-reset-recovery-20260917/source/desktop-link/build.sh
REIMS_TARGET=rpls bash video-decode-candidate/manual-runtime-discovery-20260917/build.sh
```

Do not build the backlight kext (ADL-P panel only).

## 4. Build the display bring-up, stage 3, both variants

Both builds write `display-bringup/build/ReimsDisplayBringup.kext`, so copy
each one out before the next:

```sh
REIMS_DISPLAY_BRINGUP=1 REIMS_TARGET=rpls REIMS_BRINGUP_STAGE=3 bash display-bringup/build.sh
ditto display-bringup/build/ReimsDisplayBringup.kext ~/work/bringup/stage3-dryrun/ReimsDisplayBringup.kext
REIMS_DISPLAY_BRINGUP=1 REIMS_TARGET=rpls REIMS_BRINGUP_STAGE=3 REIMS_BRINGUP_EXECUTE=1 REIMS_GT_SAMPLER=1 bash display-bringup/build.sh
ditto display-bringup/build/ReimsDisplayBringup.kext ~/work/bringup/stage3-execute/ReimsDisplayBringup.kext
```

Each build prints `built ReimsDisplayBringup.kext (stage 3 dry-run)` or
`(stage 3 EXECUTE + GT sampler)`; check you copied the right one. The
execute build carries the read-only GT sampler (`gt_sampler.hpp`): while
loaded it prints GT registers to the serial console every 250 ms when they
change, plus a 5 s heartbeat. It is a new binary, so its first load costs
one reboot.

## 5. Run the host tests

```sh
bash scripts/test.sh
```

All must pass, including `test_rpls_takeover`.

## 6. Commit the regenerated receipts

Steps 2–3 rewrite tracked files:

```sh
git add desktop-manual-20260917/fix/deferred-runtime.json \
  desktop-manual-20260917/fix/manual-gate/deferred_uuid.hpp \
  desktop-manual-20260917/fix/manual-gate-current.json \
  desktop-reset-recovery-20260917/source/desktop-link/desktop-link-current.json \
  video-decode-candidate/manual-runtime-discovery-20260917/video-discovery-current.json
git commit -m "Rebuild rpls runtime candidate and kexts for the test window"
```

`git status` should show nothing else changed by the builds.

## 7. Install the kernel components

As in [DEPLOY-SAME-HARDWARE.md](DEPLOY-SAME-HARDWARE.md) section 4, with the
candidate from `~/work/tgl-candidates`:

```sh
sudo ditto ~/work/tgl-candidates/ReimsTGLManualRuntime.kext /Library/Extensions/ReimsTGLBoot.kext
sudo ditto desktop-manual-20260917/fix/manual-gate/build/ReimsADLManualActivation.kext \
  /Library/Extensions/ReimsADLManualActivation.kext
sudo ditto desktop-reset-recovery-20260917/source/desktop-link/build/ReimsADLDesktopLink.kext \
  /Library/Extensions/ReimsADLDesktopLink.kext
sudo chown -R root:wheel /Library/Extensions/ReimsTGLBoot.kext \
  /Library/Extensions/ReimsADLManualActivation.kext /Library/Extensions/ReimsADLDesktopLink.kext
sudo chmod -R go-w /Library/Extensions/ReimsTGLBoot.kext \
  /Library/Extensions/ReimsADLManualActivation.kext /Library/Extensions/ReimsADLDesktopLink.kext
sudo kmutil install --update-all
```

Back up any existing copies first. VideoDiscovery stays in its build folder;
`igpu-start` loads it from there.

## 8. Metal bundle

`igpu-start`'s preflight verifies
`/Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle` and VideoDiscovery
before anything else, even though this window stops before the display commit.
If the bundle is not installed yet, follow DEPLOY-SAME-HARDWARE.md section 5
(`bash scripts/build-userspace.sh`, the HEVC library build, then the
`install`/`codesign` commands there).

## 9. Add `-allow3d` to the boot args

Apple's `probe` refuses any real device ID without it. In the guest's
OpenCore config (`/Volumes/EFI/EFI/OC/config.plist`, mount the EFI partition
first), add `-allow3d` to `NVRAM > Add > 7C436110-AB2A-4BBB-A880-FE41995C9F82 >
boot-args`, keeping the existing arguments. Make sure `boot-args` is listed
under `NVRAM > Delete` for that GUID, or OpenCore keeps the old value. Back up
the config and run `ocvalidate` before rebooting.

## 9b. Serial console for the GT sampler

The last window's hang blocked `ioreg` and `logd`, so the sampler reports
over serial instead. On the Proxmox host, with VM 113 stopped:

```sh
qm set 113 -serial0 socket
```

In the guest's OpenCore `boot-args`, add `serial=3` next to `-allow3d`
(kernel console on COM1: `IOLog`/`printf` output, which is what Apple's
`[IGPU]` lines and our watch use. `kprintf` additionally needs `debug=0x8`;
not required). During the window, capture it on the host from a second shell.
The socket only exists while QEMU runs (Proxmox creates it at VM start and
removes it at stop), so a plain `socat` started before `qm start` fails at
once and leaves an empty log, and one started earlier exits at the next VM
stop. Use this loop instead; start it any time, it waits for the socket and
reconnects across VM restarts (stop it with Ctrl-C):

```sh
log=/root/vm113-serial-$(date +%Y%m%d-%H%M%S).log; sock=/var/run/qemu-server/113.serial0
while :; do [ -S "$sock" ] && socat -u UNIX-CONNECT:"$sock" STDOUT; sleep 1; done | tee -a "$log"
```

The socket takes **one reader**. If `qm terminal 113` or the Proxmox web
console (serial xterm.js) is attached, `socat` reads nothing and the log
stays empty (this happened on 2026-10-14). Use `qm terminal 113` (exit with
Ctrl-O) only when you are not capturing to a file.

If the log is still empty with the VM running:

```sh
ls -l /var/run/qemu-server/113.serial0     # socket exists (type s)?
ss -xp | grep 113.serial0                  # who else is connected?
qm config 113 | grep serial                # serial0: socket
```

and in the guest `nvram boot-args` must contain `serial=3`. End-to-end test:
with the loop running, start (or reboot) the VM; kernel console lines from
the macOS boot should appear in the log within seconds. If boot output shows
but our `ReimsGT` lines do not, the problem is the kext (TEST-WINDOW "watch
the hang"), not the capture.

## 10. Reboot and check (iGPU still on the host)

Approve any new system extensions in System Settings > Privacy & Security
first if macOS asks; that needs a reboot too.

```sh
nvram boot-args
sw_vers -buildVersion
kmutil showloaded | egrep 'AppleIntelTGLGraphics|ReimsADLManualActivation|ReimsADLDesktopLink'
```

Expect `-allow3d` and `serial=3` in the boot args, build `25G83`, and the loaded
`AppleIntelTGLGraphics` UUID to be the injected candidate, `3a7cfb99…a30c`. An
earlier UUID (`89dc2f52…0dcd` = SKU+L3 only, or `edbb0eb4…af9a` = 6-byte) means
step 7 did not take the current build: redo it.

## 11. Snapshot

Shut down the guest and take a Proxmox snapshot of VM 113. The window starts
from that snapshot.
