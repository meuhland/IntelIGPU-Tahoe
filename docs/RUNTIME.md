# Runtime components and load order

ReimsVideoDiscovery is the video publication component that this project's iGPU
runtime should load. After verifying the physical device on an already
published IntelAccelerator, it publishes VideoProperties so the system media
services can discover the corresponding capabilities. Loading only the graphics
driver, or building only the HEVC library, is not a substitute for this stage.

| Component | Timing and responsibility |
| --- | --- |
| Local compatible Apple runtime | ABI-checked, deferred publication, external dependency |
| ReimsADLManualActivation | Explicitly prepares and commits the session; does not start the desktop takeover automatically |
| ReimsADLDesktopLink | Display preparation, takeover, and completion-state handling |
| ReimsADLBacklight | Built-in panel backlight |
| ReimsVideoDiscovery | Loaded after IntelAccelerator is published; verifies identity and publishes video properties |
| Metal factory / HEVC service library | User-space Metal entry; the HEVC backend is loaded by service path inside VTEncoderXPCService |

## How video publication loads

A real deployment must first meet the external-component, build-receipt,
signing, startup-isolation, and system-approval requirements in
[Same-hardware deployment and startup](DEPLOY-SAME-HARDWARE.md). Startup uses
only the root `igpu-start`; its internal state machine detects cold, prepared,
published, or video-only-missing states automatically and does not expose
staged commands.

```sh
sudo ./igpu-start
```

The runner verifies the VideoDiscovery bundle hash, signature, version, and
loaded UUID. When needed it calls `kmutil load -p <bundle>`, waits for
publication to complete, and confirms the single publisher,
PhysicalIdentityVerified, Published, and all expected video properties. If
macOS asks the user to approve, complete the system approval and rerun the same
command; an already-published display is not committed again. An unknown
runtime version is refused rather than replaced.

This public version reads the bundle from VideoDiscovery's
`build/ReimsVideoDiscovery.kext` and the pinned identity from the
`video-discovery-current.json` built in the same directory.
`prepare-deferred-runtime.py` accepts only a user-supplied, locally provided
original compatible bundle that matches the expected hash.

## Verification and recovery

After the video check, the root entry point runs the desktop-takeover checks;
when necessary it switches the old session and applies a single bounded
fallback to an original process that is stuck on exit. Save your work before
running it. The exact conditions, evidence directories, duplicate-exit
prevention, and automatic-acceptance limits are in
[Deployment and startup](DEPLOY-SAME-HARDWARE.md).

A successful publication only proves the capability is visible. It still needs
independent verification that the standard VideoToolbox service actually
encodes, with frame counts, input/output pixels, and the hardware path. The
existing HEVC first-frame anomaly is still unresolved.

An HEVC failure returns an error and isolates the context. A resource with
confirmed completion may be released; a resource whose completion is unknown is
held in a bounded isolation slot, and reuse of a failed context is refused. Do
not use fake completion or forced release to bypass the wait. On a desktop
anomaly, save pre-reset evidence first, then perform controlled recovery.
