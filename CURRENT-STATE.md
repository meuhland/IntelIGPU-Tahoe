# Current status

Confirmed on: 2026-09-21. This public snapshot contains only source and
sanitized notes.

## Last confirmed facts

- On 2026-09-28 the read-only lifetime review of GPU-stall recovery and the
  offline-candidate verification were completed, confirming that after a queue
  stop the native path still cleans up dependencies under the shared lock. The
  first cause of random stalls, complete resource holding, running callbacks,
  and out-of-lock recovery are all unfinished; no recovery patch was released
  to the public driver this time. See
  [Verified scope of GPU stall and recovery](docs/GPU-STALL-RECOVERY-STATUS.md).

- On 2026-09-21 the Apple-account-popup map black box was fixed, and the user
  confirmed the real popup map works; the new FollowUpUI loaded the
  compatibility library and hit the composite/sync path. It keeps 4x
  anti-aliasing and real GPU completion, working around the first native MSAA
  resolve coming out all-zero by averaging per sample inside the original
  command buffer. The minimal reproduction, three independent map tests, and
  post-install auto-load verification passed; the native internal first cause
  and other GPU stability are not part of the completed conclusion. See
  [Implementation and verification](mapkit-resolve-compat/README.md).

- The user has confirmed on-hardware that the one-command startup's
  desktop-takeover fix succeeded; a subsequent read-only check confirmed the
  new WindowServer and the native framebuffer at power state 2. The entry point
  now adds a detached worker process, a single bounded exit, and a
  timeout-forced-exit fallback, collecting the call stack and re-checking the
  original process's identity before the forced exit. The 10 takeover tests
  passed.
- Verification boundary: this background acceptance hit a sysmond service error
  during the process query after the session switch, so no successful
  result.json was produced; the flip count was not read this time. The
  picture recovery the user confirmed does not mean the automatic-acceptance
  chain completed, and the code does not falsely report success with a missing
  count.

- Basic Metal hardware acceleration is supported and used for WindowServer
  desktop compositing and tested application paths; ordinary textures,
  pipelines, and command submission work. General standalone Stencil8, dynamic
  shader libraries, some newer Metal features, and long-term stability are
  still limited. See [Capabilities and evidence](docs/CAPABILITIES.md).
- Historical desktop dragging is about 119–127 fps on the 144 Hz internal
  display; other fixed scenarios are about 95.94 fps, with no promise of a
  sustained 120+ fps in all scenarios.
- On 2026-09-18 the user additionally confirmed that Jianying H.264
  hardware-decoded video preview and decode acceleration during export have
  been achieved. A standalone H.264 test matched pixels for 30/30 frames; a
  sampled Jianying export was HEVC source material, hardware-decoded, with
  H.264 output, and H.264 hardware encoding has not been verified to succeed.
  See [Capabilities and evidence](docs/CAPABILITIES.md).
- The experimental machine can complete built-in display takeover; the public
  DesktopLink source version is 0.6.23.
- The video-capability publication component ReimsVideoDiscovery 0.1.3 is
  required for this runtime's video onboarding and is part of the explicit
  session flow.
- An HEVC service error no longer uses the approach of pausing the process
  indefinitely; it propagates the error and isolates the failed context. A
  referenced resource can be reclaimed only once the GPU is confirmed
  complete; when that cannot be confirmed, it stays in a bounded isolation slot
  and reuse is refused.
- Real 720p / 120-frame HEVC Main8 encoding completes; both Apple and FFmpeg
  decoders show a first-frame PSNR of about 11.63 dB and about 52.24–54.77 dB
  for the following 119 frames. This cannot be used to claim the picture is
  fully correct.
- The public source's HEVC service, ManualActivation, DesktopLink, backlight,
  and VideoDiscovery compile on Intel macOS / SDK 26.5. The kernel builds still
  have SDK-deprecation and minimum-system-version warnings.
- The complete deployment and manual-startup sequence for when the machine
  matches the verified one has been added; the local build generates identity
  receipts, which the launcher uses to strictly check the installed bundle and
  loaded UUID. Apple binaries must still be obtained legally by the user from
  the same system version.
- Manual startup has been unified into the single root `igpu-start` entry
  point. Based on the live state it runs only the missing preparation,
  display-commit, and video-verification stages, and blocks concurrent takeover
  with an exclusive lock; the old `session.py prepare/commit/video` is no
  longer a public operational interface.
- The ASan/UBSan synthetic-error recovery tests pass; the native private
  command capture is not distributed with the repository, and that part of the
  tests is explicitly skipped.

## Unresolved

The HEVC first-frame anomaly, the decisive trigger of the desktop RCS hang, and
overall stability are still unresolved. This round did not re-verify 1080p, and
did not prove all recovery paths by forcing a real hardware fault. Standalone
testing, the system VideoToolbox service, and a real Jianying export are
different verification tiers and cannot substitute for one another.

## Next steps

Locate the first-frame anomaly by keeping synchronized evidence of the
first-frame input, preprocessing, dependencies, and rebuild result; on a
natural hang, collect the command pages, mappings, and context ownership before
reset. The public build output needs the ABI, signature, and version
re-checked on the target machine before it can be deployed.
