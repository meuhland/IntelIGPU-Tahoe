#!/bin/bash
# Assemble the Reims Tahoe Metal GPUBundle (DEPLOY-SAME-HARDWARE.md section 5)
# into a staging directory. Does NOT touch /Library/GPUBundles or use sudo; it
# prints the single install command at the end.
#
# It combines the user's own, legally obtained Apple TGL Metal files with the
# three components this repo builds. The repo distributes no Apple binaries.
#
# Usage:
#   scripts/assemble-metal-bundle.sh \
#     <complete AppleIntelTGLGraphicsMTLDriver.bundle> \
#     <AppleIntelTGLGraphicsVADriver binary (becomes NativeHEVCVA)> \
#     [staging dir, default ~/work/bringup/metal-bundle]
#
# The source MTLDriver bundle must be COMPLETE: Contents/Info.plist,
# Contents/Resources, and Contents/MacOS/{AppleIntelTGLGraphicsMTLDriver,
# libigdmd.dylib}. A stripped copy with only the binaries is not enough.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$(pwd)

src=${1:?usage: assemble-metal-bundle.sh <MTLDriver.bundle> <VADriver binary> [staging dir]}
vadriver=${2:?need the AppleIntelTGLGraphicsVADriver binary (source of NativeHEVCVA)}
stage=${3:-$HOME/work/bringup/metal-bundle}
bundle="$stage/ReimsTahoeTGLGraphicsMTLDriver.bundle"
macos="$bundle/Contents/MacOS"

# The three files this repo builds (scripts/build-userspace.sh + HEVC build).
device="$repo/build/ReimsTahoeMetalDevice"
mapresolve="$repo/build/libReimsMapResolve.dylib"
hevc="$repo/hevc-encode-implementation/backend/error-recovery-20260918/build/libReimsHEVCService.dylib"

fail(){ echo "error: $*" >&2; exit 1; }

# 1. Validate inputs.
[ -f "$src/Contents/Info.plist" ] || fail "source bundle has no Contents/Info.plist (stripped copy?): $src"
[ -d "$src/Contents/Resources" ] || fail "source bundle has no Contents/Resources (stripped copy?): $src"
[ -f "$src/Contents/MacOS/AppleIntelTGLGraphicsMTLDriver" ] || fail "source bundle missing AppleIntelTGLGraphicsMTLDriver"
[ -f "$src/Contents/MacOS/libigdmd.dylib" ] || fail "source bundle missing libigdmd.dylib"
[ -f "$vadriver" ] || fail "VA driver binary not found: $vadriver"
for f in "$device" "$mapresolve" "$hevc"; do
  [ -f "$f" ] || fail "built component missing: $f (run scripts/build-userspace.sh and the HEVC build)"
done

# 2. Lay out the bundle from the complete Apple source.
rm -rf "$bundle"
mkdir -p "$macos"
ditto "$src" "$bundle"                                   # Apple MTLDriver, libigdmd, Info.plist, Resources

# 3. NativeHEVCVA = the Apple VA driver, renamed (service_bridge.c loads it by
#    this name next to libReimsHEVCService).
install -m 644 "$vadriver" "$macos/NativeHEVCVA"

# 4. The repo's components. ReimsTahoeMetalDevice is the principal class that
#    loads and patches Apple's MTLDriver.
install -m 755 "$device" "$macos/ReimsTahoeMetalDevice"
install -m 755 "$mapresolve" "$macos/libReimsMapResolve.dylib"
install -m 755 "$hevc" "$macos/libReimsHEVCService.dylib"

# 5. Point the bundle's executable and principal class at our factory, matching
#    manual-gate/profiles.plist (MetalPluginName=ReimsTahoeTGLGraphicsMTLDriver,
#    MetalPluginClassName=ReimsTahoeMetalDevice). Keep every other Apple key.
plist="$bundle/Contents/Info.plist"
plutil -replace CFBundleExecutable -string ReimsTahoeMetalDevice "$plist"
plutil -replace NSPrincipalClass   -string ReimsTahoeMetalDevice "$plist"
plutil -replace CFBundleName        -string ReimsTahoeTGLGraphicsMTLDriver "$plist"
plutil -lint "$plist" >/dev/null

# 6. Sign and verify the finished bundle.
codesign --force --deep --sign - "$bundle"
codesign --verify --deep --strict "$bundle"

echo "assembled: $bundle"
echo "contents:"; ls -1 "$macos"
echo
echo "install with:"
echo "  sudo rm -rf /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle"
echo "  sudo ditto '$bundle' /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle"
echo "  sudo chown -R root:wheel /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle"
