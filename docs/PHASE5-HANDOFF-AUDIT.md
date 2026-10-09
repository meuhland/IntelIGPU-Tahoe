# Phase 5 stage 4: handoff audit (ADL-P assumptions → RPL-S)

Upstream's native framebuffer takes over a mode the firmware lit on an ADL-P
laptop panel. On the RPL-S guest, our bring-up (`display-bringup/`, stage 3)
lights pipe A on HDMI TC1 at 3840×2160@30 with plane 1A scanning out a
GGTT-mapped framebuffer, and the firmware boot framebuffer
(`IONDRVFramebuffer` on `GFX0`) drives nothing. This audit lists every place
the handoff path assumes the ADL-P situation, with the RPL-S replacement.
Register and workaround claims are checked against Linux i915 v7.2.

## How upstream's handoff works

1. Apple's TGL accelerator (`IntelAccelerator`) is running on the IGD.
2. DesktopLink (`ReimsADLDesktopLink.cpp`) attaches to that accelerator and,
   on `NativeFramebuffer`, calls `route(true,false,true)`. `route` first
   requires the ADL-P RCS PSMI workaround to be live (`inspectRCS`,
   `rcsWorkaroundLive`), finds the single `IONDRVFramebuffer` on the PCI
   device and publishes `IOAccelDisplayPipeCapabilities`.
3. `ReimsIntelFramebufferCreate` → `configure(accel, bootFB)`: takes the
   boot framebuffer's mode, pixel info and aperture, checks the firmware-lit
   plane, decodes the running timing (`display_timing.hpp`), then retires the
   boot framebuffer (`message(0xe0016001)`, `terminate`) and attaches in its
   place.
4. The native framebuffer advertises that single mode, polls the pipe A frame
   counter for VBL, and on native `flip` validates the IOSurface layout and
   GGTT mapping, then writes `PLANE_CTL`/`PLANE_SURF` (`armPrimary`).

So the display handoff only runs with Apple's accelerator up on the GPU:
stage 4 end-to-end depends on phase 6 (TGL accelerator on RPL-S GT1).

## Kernel side (`desktop-reset-recovery-20260917/source/desktop-link/`)

| # | Where | ADL-P assumption | RPL-S replacement |
|---|---|---|---|
| K1 | `intel_framebuffer.cpp` `configure` 303–304 | Boot FB mode exactly 1920×1080, 7680 B/row, 32 bpp, from `IONDRVFramebuffer` | Mode from the port-lit pipe: 3840×2160, 15360 B/row, 32 bpp, from the bring-up's published state and the decoder |
| K2 | `configure` 307–309 | Aperture = the boot FB's system aperture | BAR2 (GMADR) sub-range at the bring-up framebuffer's GGTT offset (16 MiB), 15360 × 2160 bytes; the pages must stay wired after handoff |
| K3 | `configure` 311–313 | Firmware plane: enabled, linear, stride 120, size 1919×1079, offset 0 | Bring-up plane 1A: enabled, linear, stride `0xf0`, size `0x086f0eff`, offset 0, surface = bring-up GGTT offset |
| K4 | `configureHardwareTiming` 116–133 | Decoder is ADL-P transcoder A / PHY A; timing source string `adl-p-transcoder-a-combo-pll-link-mn` | rpls `decode(s,m)` already returns the first enabled transcoder (TC1/PHY B/DPLL0, HDMI); rename the source string per target |
| K5 | `armPrimary` 200–217, `stop` 386 | Tiled scanout toggles `CHICKEN_MISC_2[30]` to disable DPT | No DPT on display 12 (`HAS_DPT`: display ≥ 13; `intel_dpt_configure` writes `CHICKEN_MISC_DISABLE_DPT` only on display 13): skip the toggle on rpls |
| K6 | `armPrimary` 226 | Plane size write `(1079<<16)\|1919` | From the mode: `(2159<<16)\|3839` |
| K7 | `flip` 621–622 | Full-screen rect as float bits 1920.0/1080.0 (`0x44f00000`/`0x44870000`) | 3840.0/2160.0 (`0x45700000`/`0x45070000`), from the mode |
| K8 | `native_layout.hpp` `describe` | Surface must be exactly 1920×1080 | From the mode. Tiling encodings (X `1<<10`, legacy Y `4<<10`) and 256 KiB / 1 MiB base alignment are the same on display 12 |
| K9 | `getAttributeForConnection`, `getAppleSense` 471, 475 | Built-in TFT panel (`kIOConnectionBuiltIn`, `kPanelTFTConnect`); comments on `AppleBacklightDisplay` power | External HDMI: not built-in, no backlight. EDID over GMBUS (DDC pin 2) is later work (phase 8); without it macOS shows a generic display |
| K10 | `ReimsADLDesktopLink.cpp` `route` 501–505 | Handoff requires the ADL-P RCS PSMI workaround (Wa_1607297627) live | i915 applies Wa_1607297627 only on TGL, RKL and ADL-P (`intel_workarounds.c`), not ADL-S/RPL-S. On rpls the route should not require it; whether RPL-S still needs it for stability is a phase 6 question |
| K11 | `nativeFramebuffer` 556, `configure` 301 | Refused unless `kHardwareWritesPorted` | Keep: the rpls takeover writes plane registers. Flip only after this port is reviewed and the end-to-end path is tested |
| K12 | `ReimsAttachBacklight` 574 | Backlight kext for the panel | Already skipped on rpls (`kInternalPanel` false) |
| K13 | Same-register items | Pipe A frame counter `0x70040`, `TRANSCONF` A, `PLANE_*` 1A, `PLANE_SURFLIVE`, GGTT at BAR0 + 8 MiB, PTE mask | Unchanged on display 12 |

## Proposed rpls design

1. **Bring-up publishes the lit display.** After a successful stage 3 run the
   bring-up kext publishes on `GFX0` the plane state it established (GGTT
   offset, size, stride, format, transcoder, port) and keeps the framebuffer
   pages wired for as long as the plane may show them.
2. **`configure` on rpls** takes mode and pixel info from that published state,
   verifies the live registers match it (plane 1A and the decoder), builds
   the aperture from BAR2, and still retires the stub `IONDRVFramebuffer` so
   the native framebuffer occupies the same place in the registry.
3. **Mode-derived constants** replace 1920×1080 in `armPrimary`, `flip` and
   `native_layout.hpp`, with the adlp values unchanged (byte-identical adlp
   build, as for the decoder).
4. **No DPT** on rpls; **external connection** flags; **no PSMI gate** on
   rpls, recorded explicitly as following i915.
5. All of it behind `kHardwareWritesPorted`, which stays false until review
   and a test with the accelerator running.

## Runtime side (`igpu-start`, `desktop-manual-20260917/fix/`)

Flow: `igpu-start` → `session.py`: preflight (25G83, TGL candidate hash vs
`deferred-runtime.json`, gate/DesktopLink/VideoDiscovery receipts, loaded
UUIDs, `GFX0` `device-id` `ffff0000`) → `SelfTest` and `PrepareRuntime` (gate
adds the session personalities; waits for a hidden `IntelAccelerator`) →
`PrepareDisplay` (DesktopLink `PrepareRCSADLP`) → `CommitDisplay` (DesktopLink
`NativeFramebuffer`, boot FB retirement, accelerator `registerService`) →
`runtime_video.py` (VideoDiscovery) → `desktop_handoff.py` (restart
WindowServer onto the native framebuffer if it is not flipping).

The Python layer has no resolution, refresh or device-ID constants. On rpls
it stops at `PrepareDisplay`:

| # | Where | Assumption | RPL-S impact / need |
|---|---|---|---|
| R1 | `gate.cpp` 110–113, `session.py` 96–97/129/226–229, `ReimsADLDesktopLink.cpp` `link_ready`/`route` | Prepared state requires `RCSADLPWorkaroundLive` (ADL-P Wa_1607297627) and `RCSResetPSMIRegistered` | Never true on rpls (writes refused, and i915 does not apply the WA on ADL-S). Needs a per-target readiness criterion in all three places |
| R2 | `session.py` 115/126/220–223 | Exactly one `IONDRVFramebuffer`; "Firmware display disappeared before commit" | Holds while the stub IONDRV stays on `GFX0` (phase 3); wording assumes a lit firmware display |
| R3 | `desktop_handoff.py` 47/51/64/84 | The Intel framebuffer is WindowServer's display: power state 2 and completing flips; restart only from power 0 with zero flips | On the VM WindowServer also has the QXL display; the Intel output may be a secondary, idle display. Decide whether to drop QXL (`vga: none`) or accept a static secondary |
| R4 | `intel_framebuffer.cpp` 64/164–166 | Flip counters (`ReimsFlipSubmitted/Completed/Pending`) are only published when diagnostics are forced (`optimizeScanout` default true) | `desktop_handoff.py` reads them; check against upstream before relying on it (may be an upstream defect, not port-specific) |
| R5 | `profiles.plist` 115/155 | `revision-id` `DAAAAA==` (ADL-P 0x0c) | Not used by the build (`profiles.hpp` splices `REIMS_REVISION_B64`), but stale; annotate or update |
| R6 | `backlight/ReimsADLBacklight.cpp` 13 | Hard-coded `0x46a3` | Separate kext, not loaded by `session.py`; must not be installed on RPL-S |
| R7 | `runtime_video.py` 13–27 | VideoDiscovery `VideoProperties` (Gen10 codec, HEVC levels) | Unknown whether RPL-S GT1 media matches ADL-P; phase 8 |
| R8 | `graphics_control.cpp` 121 | AGDC name "Reims ADL-P integrated GPU" | Cosmetic |

## Interaction with the bring-up

`PrepareRuntime` starts Apple's TGL accelerator, which initialises the GPU
and may reprogram the GGTT, power wells or display state the bring-up set up
(GGTT `0x01000000`, pipe A, plane 1A). On ADL-P that state belonged to the
firmware and was untouched. Whether the accelerator preserves a lit pipe and
our GGTT range must be observed in phase 6; until then the handoff design
assumes the bring-up runs before `PrepareRuntime` and re-checks the plane in
`configure`.

## Blocked by phase 6

The handoff needs Apple's accelerator running on RPL-S GT1. See
"Apple TGL accelerator on RPL-S" in `PORT-RPLS-UHD770.md` for what is known
and the open question.
