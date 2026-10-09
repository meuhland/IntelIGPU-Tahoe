# IntelIGPU-Tahoe

Experimental Intel Alder Lake iGPU compatibility source for macOS Tahoe. On the
target machine it supports basic Metal hardware acceleration, display takeover,
backlight, video-capability publication, an HEVC service backend, and error
recovery.

**This is a research project that depends on the user's local Apple driver. It
is not a complete standalone GPU driver, and not a directly installable
release.** The repository does not distribute Apple binaries, modified Apple
drivers, EFI configuration, or personal diagnostic data.

Current experimental environment: Intel PCI `8086:46a3`, macOS `25G83`,
x86_64, built-in 1920×1080 / 144 Hz display link. There is no general support
guarantee for other machine models, system versions, external displays,
dynamic switch modes, or sleep/wake.

## Implemented capabilities

- **Metal is supported and used by real desktop and application paths**: the
  system can use the iGPU Metal device for WindowServer desktop compositing,
  basic texture/pipeline/command submission, and the compatibility paths that
  tested applications such as Jianying require. This conclusion refers to the
  verified compatibility scope; general standalone Stencil8, dynamic shader
  libraries, some newer Metal features, and long-term stability are still
  limited.
- **Real refresh performance of roughly 120 fps and above on the desktop**:
  the internal panel runs at 144 Hz, and desktop dragging has historically
  measured about 119–127 fps (including 126.08 / 127.05 fps). These are real
  new-frame rates for specific scenarios, not a sustained frame-rate guarantee
  for all applications; other fixed scenarios have measured about 95.94 fps.
- **Jianying H.264 hardware-decoded video preview, and decode acceleration
  during the export flow**: reported from the user's own testing. A standalone
  H.264 hardware-decode test matched the software reference pixels for 30/30
  frames; a sampled Jianying export confirmed the source material was
  hardware-decoded. "Hardware-decoded export" here means the source-decode
  stage uses hardware acceleration; hardware **encoding** of H.264 output has
  not been verified to succeed.

See [Implemented capabilities](docs/CAPABILITIES.md) for the detailed evidence
tiers and limits.

## Status and entry point

After the same-hardware build, placement, and normal macOS approval, each
reboot runs a single state-aware entry point:

```sh
sudo ./igpu-start
```

Save your work before running it: the entry point detects cold, prepared,
published, and video-only-missing states automatically, and switches the old
desktop session once if the display has not yet been taken over. After the old
WindowServer's normal-exit timeout, it first saves the call stack and
re-checks identity, then force-terminates the original process at most once.
Background acceptance requires a new session, the native framebuffer at power
state 2, and completed flips increasing; a visible picture still needs user
confirmation. Logs are in the evidence directory printed at startup; see the
deployment doc.

- [Current status and verification boundary](CURRENT-STATE.md)
- [Verified scope of GPU stall and recovery](docs/GPU-STALL-RECOVERY-STATUS.md)
- [Building from source](docs/BUILD.md)
- [Same-hardware deployment and startup](docs/DEPLOY-SAME-HARDWARE.md)
- [Runtime components and video-publication loading](docs/RUNTIME.md)
- [Source structure and release scope](docs/SOURCE-MAP.md)
- [Third-party licenses](THIRD_PARTY_NOTICES.md)

The HEVC error path has error returns, context isolation, and bounded resource
isolation; when the GPU completion state is unknown, resources must not be
force-released and completion must not be faked. The synthetic-fault tests
pass, but real 720p encoding still has a first-frame pixel anomaly, and the
desktop GPU hang is not fully resolved either. A successful capability
publication does not mean encoding or pixels are correct.

## License

The original code is **GPL-3.0-only**; see [LICENSE](LICENSE). Intel
third-party code keeps its own MIT license; paths and versions are in the
third-party notices. This grant does not cover external Apple components.
