#!/bin/bash
# Phase-6 GT-reset probe. Default = read-only (reads geometry fuses, plans the
# reset). REIMS_GT_RESET_EXECUTE=1 also issues GEN6_GDRST and re-reads — run
# only from a VM snapshot. Not installed: load on demand with
#   sudo kmutil load -p gt-reset-probe/build/ReimsGTResetProbe.kext
# and remove with sudo kmutil unload -b lab.reims.ReimsGTResetProbe.
set -euo pipefail
cd "$(dirname "$0")"
source ../scripts/target-flags.sh
flags=()
mode="read-only"
if [ "${REIMS_GT_RESET_EXECUTE:-}" = 1 ]; then flags+=(-DREIMS_GT_RESET_EXECUTE=1); mode="EXECUTE (issues GDRST)"; fi
sdk=$(xcrun --sdk macosx --show-sdk-path)
headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
out=build/ReimsGTResetProbe.kext/Contents
mkdir -p "$out/MacOS"
for src in probe module; do
 xcrun clang++ -O2 -g -arch x86_64 -mmacosx-version-min=13.0 -std=gnu++17 \
 -mkernel -fapple-kext -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector \
 -nostdinc++ -DKERNEL -DKERNEL_PRIVATE -isysroot "$sdk" -isystem "$headers" \
 -Wall -Wextra -Werror -Wno-error=deprecated-declarations \
 ${target_flags[@]+"${target_flags[@]}"} ${flags[@]+"${flags[@]}"} -c "$src.cpp" -o "build/$src.o"
done
xcrun clang++ -arch x86_64 -mmacosx-version-min=13.0 -nostdlib -isysroot "$sdk" \
 -Wl,-kext -Wl,-undefined,dynamic_lookup build/probe.o build/module.o -lkmod -lkmodc++ \
 -o "$out/MacOS/ReimsGTResetProbe"
cp Info.plist "$out/Info.plist"
codesign --force --sign - build/ReimsGTResetProbe.kext
codesign --verify --deep --strict build/ReimsGTResetProbe.kext
plutil -lint "$out/Info.plist"
echo "built ReimsGTResetProbe.kext ($mode)"
