#!/bin/bash
# Phase 5 display bring-up. WRITES DISPLAY REGISTERS when built with
# REIMS_BRINGUP_EXECUTE=1; otherwise a dry run that only reads.
# REIMS_BRINGUP_STAGE=2 (default): core init; 3: core init + HDMI TC1 4K30.
#   REIMS_DISPLAY_BRINGUP=1 REIMS_TARGET=rpls bash display-bringup/build.sh
# Load (user, fresh snapshot): stage a root-owned copy, then
#   sudo kmutil load -p /private/tmp/ReimsDisplayBringup.kext
# Read with display-bringup/read_bringup.py; unload with
#   sudo kmutil unload -b lab.reims.ReimsDisplayBringup
set -euo pipefail
cd "$(dirname "$0")"
if [ "${REIMS_DISPLAY_BRINGUP:-}" != 1 ] || [ "${REIMS_TARGET:-}" != rpls ]; then
 echo "Refusing: set REIMS_DISPLAY_BRINGUP=1 and REIMS_TARGET=rpls" >&2
 exit 64
fi
source ../scripts/target-flags.sh
stage=${REIMS_BRINGUP_STAGE:-2}
case "$stage" in 2|3) ;; *) echo "REIMS_BRINGUP_STAGE must be 2 or 3" >&2; exit 64 ;; esac
flags=(-DREIMS_DISPLAY_BRINGUP=1 -DREIMS_BRINGUP_STAGE=$stage)
mode="stage $stage dry-run"
if [ "${REIMS_BRINGUP_EXECUTE:-}" = 1 ]; then flags+=(-DREIMS_BRINGUP_EXECUTE=1); mode="stage $stage EXECUTE"; fi
sdk=$(xcrun --sdk macosx --show-sdk-path)
headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
out=build/ReimsDisplayBringup.kext/Contents
mkdir -p "$out/MacOS"
for src in bringup module; do
 xcrun clang++ -O2 -g -arch x86_64 -mmacosx-version-min=13.0 -std=gnu++17 \
 -mkernel -fapple-kext -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector \
 -nostdinc++ -DKERNEL -DKERNEL_PRIVATE -isysroot "$sdk" -isystem "$headers" \
 -Wall -Wextra -Werror -Wno-error=deprecated-declarations \
 ${target_flags[@]+"${target_flags[@]}"} "${flags[@]}" -c "$src.cpp" -o "build/$src.o"
done
xcrun clang++ -arch x86_64 -mmacosx-version-min=13.0 -nostdlib -isysroot "$sdk" \
 -Wl,-kext -Wl,-undefined,dynamic_lookup build/bringup.o build/module.o -lkmod -lkmodc++ \
 -o "$out/MacOS/ReimsDisplayBringup"
cp Info.plist "$out/Info.plist"
codesign --force --sign - build/ReimsDisplayBringup.kext
codesign --verify --deep --strict build/ReimsDisplayBringup.kext
plutil -lint "$out/Info.plist"
echo "built ReimsDisplayBringup.kext ($mode)"
