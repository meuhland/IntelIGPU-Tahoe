# Implemented capabilities and evidence boundaries

Updated: 2026-09-18. The following summarizes existing verification and the
user's own test feedback; this documentation update did not rerun the desktop
or Jianying workloads.

## Metal support

The target machine already supports basic Metal hardware acceleration: the
system and applications can obtain and use the iGPU Metal device. The verified
scope covers WindowServer desktop compositing, ordinary textures and render
pipelines, command-buffer submission, real GPU completion, and the
compatibility entry points that tested applications such as WeChat, icon
services, and Jianying require. The source entry point of the formal Metal
factory is `hevc-encode-implementation/backend/service-route-20260917/factory.m`,
along with the texture-sync, pipeline, and dynamic-library compatibility code
in the repository.

"Metal is supported" here is a capability conclusion about the measured paths
above; it does not mean the full macOS Metal interface is implemented. Known
boundaries currently include:

- General standalone Stencil8 is not done; the existing combined depth/stencil
  fallback covers only specific application scenarios.
- The managed-texture sync patch covers only verified applications and some 2D,
  single-layer resources.
- Dynamic shader libraries, binary archives, mesh, some ICB / explicit resource
  indexing, and dynamic vertex stride are faithfully reported as unsupported or
  routed through compatibility paths.
- Some Safari/icon rendering issues, reliable recovery after a GPU hang, and
  long-term stability are still unresolved.

So the project front page can clearly state "Metal is supported," but the
compatibility scope is governed by the actual verification listed here and must
not be expanded to "full support for all Metal features."

## Apple-account map black box

Passed on-hardware acceptance on 2026-09-21. The FollowUpUI-specific
compatibility keeps 4x anti-aliasing, works around the first native MSAA
resolve coming out all-zero by averaging per sample on the GPU in the original
command buffer, and preserves managed sync. The internal cause of the native
first-composite failure is not yet fully determined. See
[MapKit resolve compatibility](../mapkit-resolve-compat/README.md).

## High desktop refresh

The built-in panel runs at 1920×1080 / 144 Hz. Desktop-dragging scenarios have
recorded 119.67, 120.86, 126.08, and 127.05 fps, so this can be described as
"desktop refresh performance of roughly 120 fps and above has been achieved."
Panel refresh rate (Hz) and the actual new-frame rate generated (fps) are
different metrics.

Another fixed scenario averaged 95.942 fps over three runs, with frame-interval
P95/P99 of 14.721/20.660 ms. Different windows, sessions, and scenarios cannot
be compared directly; the results above do not mean a stable 120+ fps across
all scenarios. macOS's original visual effects are kept; the results were not
obtained by artificially limiting the frame rate. The icon anomaly and the GPU
hang remain in the unresolved scope.

## Jianying video preview and export

On 2026-09-18 the user explicitly added: Jianying H.264 hardware-decoded video
preview, and decode acceleration during export, have been achieved. This
application experience is recorded from the user's own test feedback.

Existing standalone verification: H.264 automatically selected hardware
decoding, 30 frames with no errors, and 30/30 frames matching the software
reference pixel hash. That result proves the tested H.264 decode sample; it is
not by itself a substitute for sampling the Jianying application path.

Existing Jianying export on-site verification: on 2026-09-17 the actual source
material was HEVC, the decode service ran AppleGVAHEVCDecoder → NativeTGLVA, and
real Decoded / useHighPerformanceDecode=1 appeared; the output file's sample
entry was avc1, i.e. H.264. This record confirms the source material was
hardware-decoded during export, but it cannot serve as independent path
evidence for H.264 input preview.

"Hardware-decoded export" here means **the source material is decoded with
hardware acceleration during export**. The same encoder run showed -12915 and
loaded AppleH264SW, supporting the reading that hardware-encode creation failed
and fell back to software; without an active encode stack, library loading
alone cannot prove execution. Hardware encoding of H.264 output has not been
verified to succeed; the current G12 encode backend is wired to the HEVC route.

If the verification level of the H.264 Jianying scenario needs to be raised
later, it should correlate a known H.264 input, the preview/export time window,
the actual decode calls, and the output result. The original personal videos
and diagnostic data are not published.
