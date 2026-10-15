#!/usr/bin/env python3
"""Decode the GT sampler's serial log (display-bringup/gt_sampler.hpp).

Usage: python3 display-bringup/read_gt_serial.py <serial.log>

Reads the host capture of VM 113's serial console, keeps the `ReimsGT` lines
(other kernel output may be interleaved), and prints a timeline of register
changes with decoded fields, then a summary of the final state. Read-only.
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
    "transconfA", "surfLiveA",
    "fbPteLo", "fbPteHi",
]
HEARTBEAT_MS = 5000  # Policy::heartbeatTicks x 250 ms
TICK_MS = 250
LINE_BUDGET = 4000  # Policy::maxLines
HEADER = re.compile(r"ReimsGT seq ms((?: [A-Za-z0-9]+)+)\s*$")
SAMPLE = re.compile(r"ReimsGT (\d+) (\d+)((?: [0-9a-f]{8})+)\s*$")
# gt-reset-probe hold build (gt_watch.hpp): the sampler thread stopped advancing.
STALL = re.compile(r"ReimsGT stall seq (\d+) ms (\d+) (?:reading (\w+) 0x([0-9a-f]+)|starved) for (\d+) ms")

# Forcewake: TGL's native SafeForceWake requests with index 1 (bit 1,
# ReimsADLDesktopLink.cpp kForceWakeBit); the gt-reset-probe hold build uses
# bit 2. The ack register mirrors every held requester bit; any one keeps the
# domain awake.
FORCEWAKE_BIT = 1 << 1
FORCEWAKE_REQUESTERS = 0xFFFF
# GEN11_GFX_MSTR_IRQ (i915_reg.h)
MSTR_IRQ_ENABLE, MSTR_DISPLAY, MSTR_GT_DW0, MSTR_GT_DW1 = 1 << 31, 1 << 16, 1 << 0, 1 << 1
GEN11_CSB_ENTRIES = 12
HEAD_ADDR, TAIL_ADDR, RING_VALID, RING_NR_PAGES = 0x001FFFFC, 0x001FFFF8, 0x1, 0x001FF000


def parse(text):
    names, samples = list(DEFAULT_NAMES), []
    for raw in text.splitlines():
        at = raw.find("ReimsGT")
        if at < 0:
            continue
        line = raw[at:]
        header = HEADER.match(line)
        if header:
            names = header.group(1).split()
            continue
        sample = SAMPLE.match(line)
        if not sample:
            continue
        values = [int(v, 16) for v in sample.group(3).split()]
        if len(values) != len(names):
            continue  # truncated or interleaved line
        samples.append((int(sample.group(1)), int(sample.group(2)), dict(zip(names, values))))
    return names, samples


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


def pte(sample):
    return sample["fbPteHi"] << 32 | sample["fbPteLo"]


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
    if name == "fbPteLo":
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
            changed = [n for n in s if s[n] != previous[n]]
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
    if s["rcsEir"]:
        out.append(f"  RCS error identity nonzero: EIR 0x{s['rcsEir']:08x}, ESR 0x{s['rcsEsr']:08x}.")
    if s["rcsReset"] & 0b101:
        out.append("  RCS reset requested or catastrophic error flagged.")
    if s["errorGen6"] or s["ringFault"]:
        out.append(f"  GT error registers nonzero: ERROR_GEN6 0x{s['errorGen6']:08x}, RING_FAULT 0x{s['ringFault']:08x}.")
    start = samples[0][2]
    if start["transconfA"] >> 30 & 1 and not s["transconfA"] >> 30 & 1:
        out.append("  Pipe A was active at the start and is not now: the display was turned off.")
    if s["surfLiveA"] != start["surfLiveA"]:
        out.append(f"  Plane 1A moved from 0x{start['surfLiveA']:08x} to 0x{s['surfLiveA']:08x}.")
    if pte(s) != pte(start):
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


def main(argv):
    if len(argv) != 2:
        sys.exit(__doc__.strip().splitlines()[2])
    with open(argv[1], encoding="utf-8", errors="replace") as f:
        text = f.read()
    _, samples = parse(text)
    print("\n".join(timeline(samples) + [""] + summary(samples) + stall_summary(stalls(text))))


if __name__ == "__main__":
    main(sys.argv)
