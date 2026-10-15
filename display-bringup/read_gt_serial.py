#!/usr/bin/env python3
"""Decode the GT sampler's serial log (display-bringup/gt_sampler.hpp).

Usage: python3 display-bringup/read_gt_serial.py <serial.log> [--msi rising|flat]

Reads the host capture of VM 113's serial console, keeps the `ReimsGT` lines
(other kernel output may be interleaved), and prints a timeline of register
changes with decoded fields, a summary of the final state, and a hang
verdict for RCS (executed or not; where its interrupt stopped). --msi passes
the host's /proc/interrupts vfio-msi count for 00:02.0 during the hang
(2026-10-15 window: flat, count 0). Read-only.
Field layouts follow Linux i915 (intel_engine_regs.h, gt/uc/intel_guc_reg.h).
"""
import re
import sys

# Column order of ReimsGTSampler::regs; used when the log lost the header line.
DEFAULT_NAMES = [
    "fwAckGT", "fwAckRender",
    "gucStatus", "gucScratch0", "gucDmaCtrl",
    "rcsHead", "rcsTail", "rcsStart", "rcsCtl",
    "rcsActhd", "rcsIpehr", "rcsExecStatus", "rcsCsb",
    "rcsReset", "rcsMiMode",
    "bcsHead", "bcsTail", "bcsCtl", "bcsActhd",
    "errorGen6", "ringFault",
    "gfxMstrIrq", "gtIntrDw0", "gtIntrDw1",
    "rcsBcsIntrEn", "rcsImr", "rcsEir",
    "rcsEsr", "rcsHwsPga",
    "rcsIpeir", "rcsInstdone", "rcsDmaFadd",
    "rcsBbaddr", "rcsExecStatusHi", "rcsCtxCtl",
    "faultTlb0", "faultTlb1",
    "hwsPteLo", "hwsPteHi",
    "ggttPte0Lo", "ggttPte0Hi",
    "rcsElst0Lo", "rcsElst0Hi", "rcsElst1Lo", "rcsElst1Hi",
    "rcsPdp0Lo", "rcsPdp0Hi", "rcsGfxMode", "rcsCtxStPtr",
    "lrcaPteLo", "lrcaPteHi",
    "transconfA", "surfLiveA",
    "fbPteLo", "fbPteHi",
]
HEARTBEAT_MS = 5000  # Policy::heartbeatTicks x 250 ms
TICK_MS = 250
LINE_BUDGET = 4000  # Policy::maxLines
# Kexts print each sample (and the header) as parts, "@<first column>"; older
# captures have one line per sample. IOLog's console path cuts lines at 255
# characters (xnu SERIAL_CONS_BUF_SIZE), so a one-line sample may end in a
# partial token: keep the complete columns.
HEADER = re.compile(r"ReimsGT seq ms(?: @(\d+))?((?: [A-Za-z0-9]+)+)\s*$")
SAMPLE = re.compile(r"ReimsGT (\d+) (\d+)(?: @(\d+))?((?: [0-9a-f]{8})+)(?: [0-9a-f]{1,7})?\s*$")
CONSOLE_LINE = 255
# gt-reset-probe hold build (gt_watch.hpp): the sampler thread stopped advancing.
STALL = re.compile(r"ReimsGT stall seq (\d+) ms (\d+) (?:reading (\w+) 0x([0-9a-f]+)|starved) for (\d+) ms")

# Forcewake: TGL's native SafeForceWake requests with index 1 (bit 1,
# ReimsADLDesktopLink.cpp kForceWakeBit); the gt-reset-probe hold build uses
# bit 2. The ack register mirrors every held requester bit; any one keeps the
# domain awake.
FORCEWAKE_BIT = 1 << 1
FORCEWAKE_REQUESTERS = 0xFFFF
HOLD_BIT = 1 << 2  # gt-reset-probe hold build's requester
# GEN11_GFX_MSTR_IRQ (i915_reg.h)
MSTR_IRQ_ENABLE, MSTR_DISPLAY, MSTR_GT_DW0, MSTR_GT_DW1 = 1 << 31, 1 << 16, 1 << 0, 1 << 1
GEN11_CSB_ENTRIES = 12
HEAD_ADDR, TAIL_ADDR, RING_VALID, RING_NR_PAGES = 0x001FFFFC, 0x001FFFF8, 0x1, 0x001FF000


def parse(text):
    """Return (names, samples); a sample is (seq, ms, {name: value}).

    A sample may lack trailing columns when its line was cut at the console's
    255-character limit (pre-parts captures); `truncated(samples)` reports it.
    """
    names, samples, pending = list(DEFAULT_NAMES), [], None  # pending: [seq, ms, {column: value}]

    def flush():
        if pending:
            cols = pending[2]
            samples.append((pending[0], pending[1], {names[c]: cols[c] for c in sorted(cols) if c < len(names)}))

    # A line cut at the console limit also loses its newline, so in a raw
    # capture the next ReimsGT line follows on the same line: split on every
    # occurrence, not only on line breaks.
    for line in (c.rstrip() for c in re.split(r"(?=ReimsGT )", text)):
        if not line.startswith("ReimsGT "):
            continue
        line = line.splitlines()[0]
        header = HEADER.match(line)
        if header:
            part = header.group(2).split()
            if header.group(1) is None:
                names = part
            else:
                first = int(header.group(1))
                names = names[:first] + part + names[first + len(part):]
            continue
        sample = SAMPLE.match(line)
        if not sample:
            continue
        seq, ms = int(sample.group(1)), int(sample.group(2))
        values = [int(v, 16) for v in sample.group(4).split()]
        if sample.group(3) is None:
            # One-line sample: complete, or cut at the console line limit.
            if len(values) > len(names) or (len(values) < len(names) and len(line) < CONSOLE_LINE):
                continue  # interleaved or garbled, not a console truncation
            flush()
            pending = None
            samples.append((seq, ms, dict(zip(names, values))))
            continue
        first = int(sample.group(3))
        if pending is None or pending[0] != seq:
            flush()
            pending = [seq, ms, {}]
        for i, v in enumerate(values):
            pending[2][first + i] = v
    flush()
    return names, samples


def truncated(samples, names=DEFAULT_NAMES):
    """Names missing from at least one sample (lost to line truncation)."""
    return [n for n in names if any(n not in s for _, _, s in samples)]


def guc(v):
    auth = {0: "none", 1: "BAD", 2: "good", 3: "?"}[(v >> 30) & 3]
    text = f"bootrom=0x{(v >> 1) & 0x7f:02x} ukernel=0x{(v >> 8) & 0xff:02x} mia=0x{(v >> 16) & 7:x} auth={auth}"
    return text + (" in-reset" if v & 1 else "")


def ring(s, prefix):
    head, tail, ctl = s[prefix + "Head"] & HEAD_ADDR, s[prefix + "Tail"] & TAIL_ADDR, s[prefix + "Ctl"]
    if not ctl & RING_VALID:
        return "off"
    size = (((ctl & RING_NR_PAGES) >> 12) + 1) * 4096
    if head == tail:
        return f"on, idle (head=tail=0x{head:x})"
    return f"on, {(tail - head) % size} bytes queued (head=0x{head:x} tail=0x{tail:x})"


def reset(v):
    flags = [n for bit, n in ((0, "request"), (1, "ready"), (2, "CATASTROPHIC")) if v >> bit & 1]
    return ",".join(flags) or "none"


def mimode(v):
    flags = [n for bit, n in ((8, "stop-ring"), (9, "idle")) if v >> bit & 1]
    return ",".join(flags) or "running"


def pte(sample, prefix="fbPte"):
    return sample[prefix + "Hi"] << 32 | sample[prefix + "Lo"]


def pte_address(sample, prefix):
    """Physical address of a Gen12 64-bit GGTT PTE (bits 38:12; bit 0 = valid)."""
    return pte(sample, prefix) & 0x7FFFFFF000


def csb(v):
    # RING_CONTEXT_STATUS_PTR: read pointer bits 11:8, write pointer bits 3:0,
    # 12 entries on Gen11+ (intel_lrc_reg.h GEN11_CSB_*).
    read, write = (v >> 8) & 0xF, v & 0xF
    if read >= GEN11_CSB_ENTRIES or write >= GEN11_CSB_ENTRIES:
        return read, write, None
    return read, write, (write - read) % GEN11_CSB_ENTRIES


def describe(name, value):
    if name.startswith("fwAck"):
        held = value & FORCEWAKE_REQUESTERS
        return f"0x{value:08x} (" + (f"acked, requester bits 0x{held:x}" if held else "NOT acked") + ")"
    if name == "gucStatus":
        return f"0x{value:08x} ({guc(value)})"
    if name == "rcsReset":
        return f"0x{value:08x} ({reset(value)})"
    if name == "rcsCsb":
        read, write, unread = csb(value)
        return f"0x{value:08x} (read={read} write={write}" + (f", {unread} unread)" if unread is not None else ", reset)")
    if name == "rcsMiMode":
        return f"0x{value:08x} ({mimode(value)})"
    if name.endswith("PteLo"):
        if value == 0xFFFFFFFF and name == "hwsPteLo":
            return "0xffffffff (outside the mapped BAR)"
        return f"0x{value:08x} ({'valid' if value & 1 else 'NOT valid'}, address bits 0x{value & 0xFFFFF000:08x})"
    if name == "gfxMstrIrq":
        bits = [n for b, n in ((MSTR_GT_DW0, "GT DW0"), (MSTR_GT_DW1, "GT DW1"), (MSTR_DISPLAY, "display"))
                if value & b]
        return (f"0x{value:08x} (master {'enabled' if value & MSTR_IRQ_ENABLE else 'DISABLED'}, pending: "
                + (", ".join(bits) or "none") + ")")
    if name == "transconfA":
        return f"0x{value:08x} ({'enabled' if value >> 31 & 1 else 'disabled'}, {'active' if value >> 30 & 1 else 'inactive'})"
    return f"0x{value:08x}"


def timeline(samples):
    out, previous = [], None
    for seq, ms, s in samples:
        if previous is None:
            out.append(f"{ms:>9} ms  #{seq}  first sample")
            out.extend(f"{'':14}{n} = {describe(n, v)}" for n, v in s.items())
        else:
            changed = [n for n in s if n in previous and s[n] != previous[n]]
            out.append(f"{ms:>9} ms  #{seq}  " + (f"{len(changed)} changed" if changed else "heartbeat (no change)"))
            out.extend(f"{'':14}{n}: {describe(n, previous[n])} -> {describe(n, s[n])}" for n in changed)
        previous = s
    return out


def summary(samples):
    if not samples:
        return ["No ReimsGT samples found (sampler not built in, not loaded, or serial=3 missing)."]
    out = []
    first_ms, last_ms = samples[0][1], samples[-1][1]
    last_change = first_ms
    for (_, _, a), (_, ms, b) in zip(samples, samples[1:]):
        if a != b:
            last_change = ms
    out.append(f"{len(samples)} samples from {first_ms} ms to {last_ms} ms; last register change at {last_change} ms"
               f" ({(last_ms - last_change) / 1000:.1f} s before the end of the log).")
    if len(samples) >= LINE_BUDGET:
        out.append(f"The sampler's {LINE_BUDGET}-line budget was reached: later changes were not logged.")
    gaps = [(b[1] - a[1], a[1], b[0] - a[0]) for a, b in zip(samples, samples[1:])]
    worst = max(gaps, default=(0, 0, 0))
    if worst[0] > 2 * HEARTBEAT_MS:
        expected = worst[0] // TICK_MS
        if worst[2] < expected // 2:
            out.append(f"Sampler went silent for {worst[0] / 1000:.1f} s after {worst[1]} ms and its timer ran only "
                       f"{worst[2]} of ~{expected} ticks: its work loop or the CPU was blocked, not only IOKit.")
        else:
            out.append(f"No line for {worst[0] / 1000:.1f} s after {worst[1]} ms although the timer kept running "
                       f"({worst[2]} ticks): lines were lost on the serial path.")

    s = samples[-1][2]
    out.append("Final state:")
    if not (s["fwAckGT"] & FORCEWAKE_REQUESTERS and s["fwAckRender"] & FORCEWAKE_REQUESTERS):
        out.append("  forcewake: not acknowledged on GT and/or Render (no requester bit held); GT registers may "
                   "read 0 because the GT is asleep, or a forcewake request was never acknowledged.")
    else:
        out.append(f"  forcewake: acknowledged (GT awake; GT requester bits 0x{s['fwAckGT'] & FORCEWAKE_REQUESTERS:x}).")
    out.append(f"  GuC: {describe('gucStatus', s['gucStatus'])}; DMA_CTRL 0x{s['gucDmaCtrl']:08x}")
    if (s["gucStatus"] >> 30) & 3 == 1:
        out.append("  GuC firmware authentication FAILED.")
    out.append(f"  RCS: {ring(s, 'rcs')}; ACTHD 0x{s['rcsActhd']:08x}; reset {reset(s['rcsReset'])}; "
               f"MI_MODE {mimode(s['rcsMiMode'])}")
    out.append(f"  BCS: {ring(s, 'bcs')}; ACTHD 0x{s['bcsActhd']:08x}")
    for engine in ("rcs", "bcs"):
        tail = [x[2] for x in samples[-3:]]
        busy = all(x[engine + "Head"] & HEAD_ADDR != x[engine + "Tail"] & TAIL_ADDR and x[engine + "Ctl"] & RING_VALID
                   for x in tail)
        if len(tail) >= 2 and busy and len({x[engine + "Acthd"] for x in tail}) == 1:
            out.append(f"  {engine.upper()} has queued work and ACTHD is not moving over the last {len(tail)} "
                       "samples: the engine is stalled.")
    trailing = 0
    for x in reversed(samples):
        if x[2]["rcsCsb"] != s["rcsCsb"]:
            break
        trailing += 1
    unread = csb(s["rcsCsb"])[2]
    if trailing >= 2 and unread:
        out.append(f"  RCS context-status buffer has {unread} unread entries that are not being consumed "
                   "(execlist events not processed).")
    tail = [x[2] for x in samples[-3:]]
    pending = [x for x in tail if x["gtIntrDw0"] or x["gtIntrDw1"]]
    if len(tail) >= 2 and len(pending) == len(tail):
        out.append(f"  GT interrupt pending and not serviced over the last {len(tail)} samples (GT_INTR_DW0 "
                   f"0x{s['gtIntrDw0']:08x}, DW1 0x{s['gtIntrDw1']:08x}; {describe('gfxMstrIrq', s['gfxMstrIrq'])}): "
                   "the GPU raised it, nothing handled it.")
    if s.get("rcsEir"):
        out.append(f"  RCS error identity nonzero: EIR 0x{s['rcsEir']:08x}, ESR 0x{s.get('rcsEsr', 0):08x}.")
    if s["rcsReset"] & 0b101:
        out.append("  RCS reset requested or catastrophic error flagged.")
    if s["errorGen6"] or s["ringFault"]:
        out.append(f"  GT error registers nonzero: ERROR_GEN6 0x{s['errorGen6']:08x}, RING_FAULT 0x{s['ringFault']:08x}.")
    if s.get("faultTlb0") or s.get("faultTlb1"):
        out.append(f"  GT fault TLB data: 0x{s.get('faultTlb1', 0):08x}_{s.get('faultTlb0', 0):08x}.")
    if samples[0][2]["fwAckGT"] & HOLD_BIT and not s["fwAckGT"] & HOLD_BIT:
        out.append("  The hold probe's forcewake bit 2 was cleared while it was loaded (something reset all "
                   "forcewake requests); the GT then stays awake only on other requesters.")
    if all(k in s for k in ("rcsHwsPga", "hwsPteLo", "hwsPteHi")) and s["hwsPteLo"] != 0xFFFFFFFF:
        valid = s["hwsPteLo"] & 1
        addr = pte_address(s, "hwsPte")
        out.append(f"  RCS status page: GGTT 0x{s['rcsHwsPga'] & 0xFFFFF000:08x} -> PTE "
                   f"0x{pte(s, 'hwsPte'):016x} ({'valid' if valid else 'NOT valid'}, physical 0x{addr:x})"
                   + (": the GPU's status-page writes go to physical 0 (cf. a host DMAR write at 0x0)."
                      if valid and addr == 0 else "."))
    if all(k in s for k in ("rcsElst0Lo", "lrcaPteLo", "lrcaPteHi")):
        lrca = s["rcsElst0Lo"] & 0xFFFFF000
        if s["lrcaPteLo"] == 0xFFFFFFFF:
            out.append(f"  RCS context (ELST0 0x{s['rcsElst0Lo']:08x}, LRCA 0x{lrca:08x}): PTE outside the mapped BAR.")
        else:
            valid, addr = s["lrcaPteLo"] & 1, pte_address(s, "lrcaPte")
            out.append(f"  RCS context (ELST0 0x{s['rcsElst0Hi']:08x}_{s['rcsElst0Lo']:08x}, LRCA GGTT 0x{lrca:08x}) "
                       f"-> PTE 0x{pte(s, 'lrcaPte'):016x} ({'valid' if valid else 'NOT valid'}, physical 0x{addr:x})"
                       + (": the context image is read from and saved to physical 0 (cf. the DMAR write at 0x0)."
                          if valid and addr == 0 else
                          ("; no context submitted (ELST0 is 0)." if lrca == 0 else ".")))
    if all(k in s for k in ("rcsPdp0Lo", "rcsPdp0Hi")):
        root = (s["rcsPdp0Hi"] << 32 | s["rcsPdp0Lo"]) & 0x7FFFFFF000
        out.append(f"  RCS PPGTT root (PDP0 descriptor, PML4 for 4-level): 0x{s['rcsPdp0Hi']:08x}_{s['rcsPdp0Lo']:08x}"
                   + (" -> page walks start at physical 0." if root == 0 and "rcsElst0Lo" in s and s["rcsElst0Lo"] else
                      f" -> physical 0x{root:x}."))
    if all(k in s for k in ("ggttPte0Lo", "ggttPte0Hi")):
        out.append(f"  GGTT entry 0: 0x{pte(s, 'ggttPte0'):016x} "
                   f"({'valid' if s['ggttPte0Lo'] & 1 else 'NOT valid'}, physical 0x{pte_address(s, 'ggttPte0'):x}).")
    start = samples[0][2]
    missing = truncated(samples)
    if missing:
        out.append(f"  {len(missing)} columns missing from some samples (cut at the console's "
                   f"{CONSOLE_LINE}-character line limit, or not sampled by that build): {', '.join(missing)}.")
    if "transconfA" in s and "transconfA" in start and start["transconfA"] >> 30 & 1 and not s["transconfA"] >> 30 & 1:
        out.append("  Pipe A was active at the start and is not now: the display was turned off.")
    if "surfLiveA" in s and "surfLiveA" in start and s["surfLiveA"] != start["surfLiveA"]:
        out.append(f"  Plane 1A moved from 0x{start['surfLiveA']:08x} to 0x{s['surfLiveA']:08x}.")
    if all(k in x for x in (s, start) for k in ("fbPteLo", "fbPteHi")) and pte(s) != pte(start):
        out.append(f"  GGTT entry of the bring-up's first framebuffer page changed from 0x{pte(start):016x} to "
                   f"0x{pte(s):016x}: the GGTT range was reused or cleared.")
    return out


def stalls(text):
    """(seq, ms, register name or None if starved, offset, stuck ms) per stall line."""
    out = []
    for m in STALL.finditer(text):
        out.append((int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4), 16) if m.group(4) else None,
                    int(m.group(5))))
    return out


def stall_summary(found):
    if not found:
        return []
    seq, ms, name, offset, stuck = found[-1]
    if name:
        return [f"Sampler STUCK in a register read: {name} (0x{offset:06x}) never completed; last report "
                f"{stuck / 1000:.1f} s after sample #{seq} (log time {ms} ms). An MMIO read to the GT is hanging, "
                "so the accelerator's own thread is likely stuck the same way."]
    return [f"Sampler thread STARVED (not in a read) for {stuck / 1000:.1f} s after sample #{seq}: the CPU it "
            "needs is not being scheduled (a spinning thread with preemption disabled?)."]


# GT interrupt bits (i915 intel_gt_regs.h), per engine half: RCS in bits
# 31:16 of GEN11_RENDER_COPY_INTR_ENABLE, BCS in 15:0; the same bit numbers
# in the engine's RING_IMR (set = masked).
GT_USER, GT_PIPECTL_NOTIFY, GT_CONTEXT_SWITCH = 1 << 0, 1 << 4, 1 << 8
GT_BITS = ((GT_USER, "USER"), (GT_PIPECTL_NOTIFY, "PIPECTL_NOTIFY"), (GT_CONTEXT_SWITCH, "CONTEXT_SWITCH"))


def bits_named(v):
    return ", ".join(n for b, n in GT_BITS if v & b) or "none"


def hang_verdict(samples, msi_count_rose=None):
    """Answer: did RCS execute, and if so, where did its interrupt stop?

    Three readings decide it (ACTHD movement, GT_INTR_DW0, master bit 31),
    then the engine-level enable and IMR. msi_count_rose: the host's
    /proc/interrupts vfio-msi count for 00:02.0 during the hang (True/False),
    or None if not measured.
    """
    if not samples:
        return []
    s, first = samples[-1][2], samples[0][2]
    acthd = [x[2]["rcsActhd"] for x in samples]
    moved = len(set(acthd)) > 1
    tail = [x[2] for x in samples[-3:]]
    moving_now = len({x["rcsActhd"] for x in tail}) > 1
    head, tl, ctl = s["rcsHead"] & HEAD_ADDR, s["rcsTail"] & TAIL_ADDR, s["rcsCtl"]
    valid = bool(ctl & RING_VALID)
    drained = valid and head == tl
    dw0_end = s["gtIntrDw0"]
    dw0_ever = any(x[2]["gtIntrDw0"] for x in samples)
    master_end = bool(s["gfxMstrIrq"] & MSTR_IRQ_ENABLE)
    master_ever = any(x[2]["gfxMstrIrq"] & MSTR_IRQ_ENABLE for x in samples)
    rcs_enable = s["rcsBcsIntrEn"] >> 16
    imr_masked = s["rcsImr"] & (GT_USER | GT_PIPECTL_NOTIFY | GT_CONTEXT_SWITCH)
    # RING_RESET_CTL (i915 intel_engine_regs.h): bit 0 REQUEST_RESET, 1 READY_TO_RESET,
    # 2 CAT_ERROR. For CAT_ERROR i915 bypasses the ready-to-reset handshake
    # (intel_reset.c, HAS#396813) and lets the hardware clear the error.
    cat_error = bool(s["rcsReset"] & 0b100)
    resets = [x[2]["rcsReset"] for x in samples]
    reset_requests = sum(1 for a, b in zip(resets, resets[1:]) if b & 1 and not a & 1)
    ever_ready = any(r & 0b10 for r in resets)
    csb_writes = len({x[2]["rcsCsb"] & 0xF for x in samples if x[1] >= next(
        (y[1] for y in samples if y[2]["gfxMstrIrq"] & MSTR_IRQ_ENABLE), samples[0][1])}) > 1

    out = ["Hang verdict (RCS):",
           f"  ACTHD: 0x{first['rcsActhd']:08x} -> 0x{s['rcsActhd']:08x}, "
           + ("moved during the capture" if moved else "NEVER moved")
           + ("; still moving at the end" if moving_now else "; frozen at the end"),
           f"  ring: {ring(s, 'rcs')}",
           f"  GT_INTR_DW0: 0x{dw0_end:08x} at the end" + ("" if dw0_end or not dw0_ever else " (nonzero earlier)"),
           f"  master IRQ (0x190010 bit 31): {'set' if master_end else 'CLEAR'} at the end"
           + ("" if master_end == master_ever else " (set earlier)"),
           f"  RCS interrupt enable: {bits_named(rcs_enable)}; RCS IMR masks: {bits_named(imr_masked)}"]
    out.append(f"  RING_RESET_CTL: {reset(s['rcsReset'])} at the end; {reset_requests} reset request(s) seen, "
               + ("READY_TO_RESET seen" if ever_ready else "READY_TO_RESET never seen"))
    out.append("  context-status buffer write pointer " + ("advanced" if csb_writes else
               "never advanced (no context-switch event)") + " after interrupts were enabled")
    if msi_count_rose is not None:
        out.append(f"  host vfio-msi count during the hang: {'rising' if msi_count_rose else 'did not move'}")
    if cat_error:
        out.append("  RCS reports a CATASTROPHIC error. "
                   + (f"Someone requested an engine reset {reset_requests} time(s) with the normal "
                      "ready-to-reset handshake, which cannot complete under CAT_ERROR (i915 bypasses it: "
                      "HAS#396813), so recovery never finishes."
                      if reset_requests and not ever_ready else "No reset completed."))

    if not valid and cat_error:
        out.append("=> NEVER EXECUTED, not an interrupt problem: no context ever loaded on RCS (ring registers "
                   "and ACTHD stay 0, no context-switch event) and the engine flagged a catastrophic error as "
                   "the submission happened. The first context/ring the accelerator submitted was rejected "
                   "(look at the error identity, fault and execlist registers); the interrupt path is armed.")
    elif not valid:
        out.append("=> No context is running on RCS (ring not valid): the stamps were never submitted to the "
                   "hardware, or the context never loaded. Never executed; CPU/submission side.")
    elif not moved and not drained:
        out.append("=> RCS has work queued and ACTHD never moved: the engine never executed the stamps. "
                   "GPU side (context/ring/memory setup), not interrupts.")
    elif moving_now and not drained:
        out.append("=> RCS is still executing at the end of the capture (ACTHD moving, work queued): slow or "
                   "looping, not stalled.")
    elif dw0_end and not master_end:
        out.append("=> Executed, and a GT interrupt is pending in GT_INTR_DW0, but the master enable is clear: "
                   "the interrupt is masked at the master; nothing can raise the MSI.")
    elif dw0_end and master_end:
        out.append("=> Executed, a GT interrupt is pending and the master is enabled"
                   + (", yet the MSI count did not move: the interrupt is not reaching the MSI."
                      if msi_count_rose is False else ": check the host MSI count to see whether it reached the CPU."))
    elif not rcs_enable & (GT_PIPECTL_NOTIFY | GT_USER) or imr_masked & (GT_PIPECTL_NOTIFY | GT_USER):
        out.append("=> Executed (ring drained) with no interrupt pending, and the completion interrupt is not "
                   "enabled at the engine (enable register or RING_IMR): masked at the engine.")
    else:
        out.append("=> Executed (ring drained), interrupts enabled at the engine and master, nothing pending: "
                   "the stamps did not ask for an interrupt, or one was raised and already serviced"
                   + (" (but the MSI count did not move, so not serviced)." if msi_count_rose is False else "."))
    return out


def main(argv):
    args = argv[1:]
    msi = None
    if len(args) == 3 and args[1] == "--msi" and args[2] in ("rising", "flat"):
        msi = args[2] == "rising"
        args = args[:1]
    if len(args) != 1:
        sys.exit(__doc__.strip().splitlines()[2])
    with open(args[0], encoding="utf-8", errors="replace") as f:
        text = f.read()
    _, samples = parse(text)
    print("\n".join(timeline(samples) + [""] + summary(samples) + stall_summary(stalls(text))
                    + [""] + hang_verdict(samples, msi)))


if __name__ == "__main__":
    main(sys.argv)
