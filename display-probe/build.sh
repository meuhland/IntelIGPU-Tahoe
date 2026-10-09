#!/bin/bash
# Read-only display probe. Not installed: load on demand with
#   sudo kmutil load -p display-probe/build/ReimsDisplayProbe.kext
# and remove with sudo kmutil unload -b lab.reims.ReimsDisplayProbe.
set -euo pipefail
cd "$(dirname "$0")"
source ../scripts/target-flags.sh
sdk=$(xcrun --sdk macosx --show-sdk-path)
headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
out=build/ReimsDisplayProbe.kext/Contents
mkdir -p "$out/MacOS"
for src in probe module; do
 xcrun clang++ -O2 -g -arch x86_64 -mmacosx-version-min=13.0 -std=gnu++17 \
 -mkernel -fapple-kext -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector \
 -nostdinc++ -DKERNEL -DKERNEL_PRIVATE -isysroot "$sdk" -isystem "$headers" \
 -Wall -Wextra -Werror -Wno-error=deprecated-declarations \
 ${target_flags[@]+"${target_flags[@]}"} -c "$src.cpp" -o "build/$src.o"
done
xcrun clang++ -arch x86_64 -mmacosx-version-min=13.0 -nostdlib -isysroot "$sdk" \
 -Wl,-kext -Wl,-undefined,dynamic_lookup build/probe.o build/module.o -lkmod -lkmodc++ \
 -o "$out/MacOS/ReimsDisplayProbe"
cp Info.plist "$out/Info.plist"
codesign --force --sign - build/ReimsDisplayProbe.kext
codesign --verify --deep --strict build/ReimsDisplayProbe.kext
plutil -lint "$out/Info.plist"
python3 ../scripts/write-kext-receipt.py \
 build/ReimsDisplayProbe.kext ReimsDisplayProbe display-probe-current.json
