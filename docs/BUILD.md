# Build

Requires Intel macOS, the Xcode Command Line Tools (including the
Kernel.framework headers), and Python 3. Verified with SDK 26.5; the private
ABI is not guaranteed to be compatible across system versions. All commands
below run from the repository root and do not install or load any driver.

```sh
bash scripts/build-userspace.sh
python3 hevc-encode-implementation/backend/error-recovery-20260918/build.py
bash desktop-manual-20260917/fix/manual-gate/build.sh
bash video-decode-candidate/manual-runtime-discovery-20260917/build.sh
bash desktop-reset-recovery-20260917/source/backlight/build.sh
REIMS_TGL_IMAGE=/path/to/local/AppleIntelTGLGraphics bash desktop-reset-recovery-20260917/source/desktop-link/build.sh
```

The last command requires a locally supplied, compatible AppleIntelTGLGraphics,
used only for ABI verification; the repository does not include that file. The
build script rejects any input that does not satisfy the ABI contract. Output
goes to each component's `build/` and to the root `build/`; the control tool's
output is in manual-gate/control.

```sh
bash scripts/test.sh
```

The tests cover ASan/UBSan HEVC synthetic faults, the video publication flow,
and the collector's events and retention windows. Native command capture is
not published, so tests that depend on that private sample are explicitly
skipped; passing tests do not mean that real hardware faults, actual exports,
or pixel verification have passed.

## Deployment boundary

There is no general one-click installer. The build scripts write a
`*-current.json` for each locally built kext containing its SHA-256 and UUID;
the session scripts verify the installed and loaded versions against this
build's receipts, the system version, and the physical device identity. Do not
remove these checks in order to load.

The complete same-hardware build, placement, approval, manual takeover,
verification, and recovery sequence is in
[Same-hardware deployment and startup](DEPLOY-SAME-HARDWARE.md). Use the normal
macOS kernel-extension approval flow; do not manually overwrite the AuxKC. Only
deploy for real after the original bundles and a recovery path are preserved.
