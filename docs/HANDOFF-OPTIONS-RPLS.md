# RPL-S display handoff: design options after the first accelerator start

Status: design note, 2026-10-13. No code depends on it yet. Read with
[PHASE5-HANDOFF-AUDIT.md](PHASE5-HANDOFF-AUDIT.md) (current design, K/R
items) and [PORT-RPLS-UHD770.md](PORT-RPLS-UHD770.md) ("First accelerator
start on `a780`").

## The current design and what broke it

As written, the handoff assumes this order:

1. `display-bringup` (stage 3) lights pipe A on HDMI TC1 and points plane 1A
   at its own framebuffer, which it maps by writing GGTT entries for 8100
   pages (at `0x01000000` until 2026-10-15; now the first unused range from
   16 MiB up, `ReimsBringupFramebufferGGTT`). The kext stays loaded and owns
   those pages.
2. `igpu-start` → `PrepareRuntime` starts Apple's accelerator, hidden.
3. `CommitDisplay` → DesktopLink's native framebuffer finds plane 1A still
   lit, takes its mode and aperture (`rpls_takeover.hpp`) and starts flipping
   WindowServer surfaces onto it.

The 2026-10-13 window got through step 2's probe and start, then the screen
went black and IOKit wedged. So step 3's precondition, "the bring-up's plane
is still lit after `PrepareRuntime`", did not hold. The hang must be fixed
before any handoff runs, but whichever way it is fixed, the blanking needs
an answer too.

What we do not know yet is *why* it went black. Candidates:

- **GGTT reuse.** The accelerator initialises and allocates from the GGTT;
  nothing reserves the bring-up's range, so its PTEs may be rewritten. Pipe
  and plane stay on but fetch other memory (black or garbage).
- **Display programming.** `IntelAccelerator::start` calls
  `registerWithFramebufferController` and touches power and forcewake; if
  anything turns off pipe A, its power well or DBUF, the pipe stops.
- **The hang itself.** A wedged GT does not normally stop the display
  engine, but a reset or power-domain change on the way could.

## Options

### A. Keep the order and make the lit plane survive

Bring-up first, as now; protect its state across `PrepareRuntime` or repair
it afterwards.

- Needs a GGTT range the accelerator will not hand out (reserved or
  stolen-backed), or a check-and-repair of the PTEs after start.
- If the accelerator turns the pipe off, A becomes "re-light after start",
  which is option B.
- Smallest change to the existing code and receipts. Fragile: it depends on
  private behaviour of Apple's GGTT allocator, and every later accelerator
  reset can repeat the problem.

### B. Light the display after `PrepareRuntime`

Start the accelerator hidden first, then run the stage-3 sequence, then
commit.

- The bring-up's preconditions refuse this today (only `IONDRVFramebuffer`
  may be attached), so they would become "accelerator present, hidden and
  idle".
- The framebuffer should come from the accelerator rather than private GGTT
  writes, otherwise it is the same collision as A.
- Avoids being undone by start. Still two display owners in sequence
  (bring-up, then the native framebuffer), and still a separate kext that
  must stay loaded.

### C. The native framebuffer lights the display itself

Move the stage-2/3 sequences (`core_init.hpp`, `hdmi_output.hpp`, the plane
part of `hdmi_scanout.hpp`) behind DesktopLink's native framebuffer: at
commit it powers the display, lights pipe A and scans out the first
WindowServer surface, whose GGTT mapping Apple's driver already owns.

- One display owner from commit onward, no private GGTT range, no extra
  kext to keep loaded. Sleep/wake, mode changes and hotplug need this
  ownership eventually anyway.
- The headers are already templated over an IO interface and host-tested,
  so they can move without a rewrite; `rpls_takeover.hpp`'s plane check
  becomes a post-condition instead of a precondition.
- Most work, and it puts display register writes inside DesktopLink, which
  makes `kHardwareWritesPorted` review larger. The display would only light
  once the accelerator is up, so a hang leaves a black screen (the serial
  sampler still reports).
- **Started (code, opt-in, 2026-10-13):** `rpls_light.hpp` runs the stage 2/3
  sequence over DesktopLink's BAR0 and its own black framebuffer
  (`fb_memory.hpp`, shared with the bring-up kext). `configure` calls it when
  the plane is not already lit, then re-reads. Gated behind
  `REIMS_RPLS_SELF_LIGHT` (off): the committed rpls `intel_framebuffer.o` is
  byte-identical to before. Still to do before enabling: take the first
  framebuffer from Apple's allocator instead of a private page set, confirm
  the ordering against `PrepareRuntime` on hardware, and the
  `kHardwareWritesPorted` review.

### D. Apple's TGL framebuffer kext (the NootedGreen route)

Load `AppleIntelTGLGraphicsFramebuffer` and remap its port B to TC1, as in
the local NootedGreen `rpls-uhd770` branch. Listed for completeness: it
discards this project's design (FFFF isolation, deferred publication, native
framebuffer), depends on Apple's framebuffer handling a cold RPL-S display
engine through a register remap, and NootedGreen's licence keeps its code
out of this repository. Not recommended here.

## What the next window can decide

The GT sampler already logs `transconfA` and `surfLiveA` across
`PrepareRuntime`, which separates the first two causes:

| Sampler shows when the screen goes black | Likely cause | Points to |
|---|---|---|
| Pipe A still active, `surfLiveA` unchanged | Content changed under the plane: GGTT entries or pages reused | A only with a reserved range; otherwise B/C |
| Pipe A inactive | Something turned the display off | B or C (light after start) |
| `surfLiveA` moved | Something reprogrammed plane 1A | B or C; find the writer |
| Screen stays lit until the hang | Blanking is a side effect of the hang | Fix the hang, re-test A |

The sampler also logs the GGTT PTE of the bring-up's first framebuffer page
(`fbPteLo`/`fbPteHi`, BAR0 `0x800000` + (`0x01000000` >> 12) × 8 =
`0x808000`), which makes the first row certain: if it changes during
`PrepareRuntime`, the accelerator reused the range, and `read_gt_serial.py`
says so in its summary. That watch is fixed at GGTT `0x01000000`, so it only
covers the bring-up's page when `ReimsBringupFramebufferGGTT` is `0x1000000`.

## Recommendation

Fix the hang first; nothing here runs before the accelerator stays up.
Then aim for **C**, the long-term shape: one owner, Apple-mapped surfaces,
and the place mode changes and sleep/wake must live anyway. Use the next
window's sampler output to decide whether an interim **A** (cheap, if the
cause is only GGTT reuse) is worth doing first to get a first desktop
sooner.
