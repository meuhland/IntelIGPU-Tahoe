#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 hevc-encode-implementation/backend/error-recovery-20260918/test.py
python3 desktop-manual-20260917/fix/test_session.py
python3 desktop-manual-20260917/fix/test_desktop_handoff.py
python3 desktop-manual-20260917/fix/runtime-video-20260918/test_runtime_video.py
(cd desktop-reset-recovery-20260917/collector-v5 && python3 test_events.py && python3 test_store.py)
timing_test=$(mktemp -d)
trap 'rm -rf "$timing_test"' EXIT
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -DREIMS_TARGET_RPLS \
 desktop-reset-recovery-20260917/source/desktop-link/test_display_timing.cpp \
 -o "$timing_test/test_display_timing"
"$timing_test/test_display_timing"
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -DREIMS_DISPLAY_BRINGUP=1 -DREIMS_TARGET_RPLS \
 display-bringup/test_core_init.cpp -o "$timing_test/test_core_init"
"$timing_test/test_core_init"
