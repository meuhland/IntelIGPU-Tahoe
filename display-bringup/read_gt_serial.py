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
    "rcsSq0Lo", "rcsSq0Hi", "rcsSq1Lo", "rcsSq1Hi",
    "rcsGfxMode",
    "rcsCcid", "rcControl", "rcState", "pgEnable",
    "rc6Location", "rc6CtxBase",
    "lrcaPteLo", "lrcaPteHi",
    "pocsReset", "pocsExecStatus", "pocsMiMode",
    "pocsHwsPga", "pocsCtxCtl",
    "ccsReset", "ccsExecStatus", "rcuMode",
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
# gt-reset-probe hold build (gt_ctxdump.hpp): the submitted context's pages.
CTX_PTE = re.compile(r"ReimsGT ctx ([0-9a-f]{8}) p(\d) pte ([0-9a-f]{16})")
CTX_WORDS = re.compile(r"ReimsGT ctx ([0-9a-f]{8}) p(\d) @(\d+)((?: [0-9a-f]{8})+)")
# probe.cpp prints these instead of a page: "PPGTT root is 0", "map failed", "pte outside BAR"
CTX_NOTE = re.compile(r"ReimsGT ctx ([0-9a-f]{8}) p(\d) (PPGTT root is 0|map failed|pte outside BAR)")
# MI_LOAD_REGISTER_IMM (i915 intel_gpu_commands.h): opcode 0x22 in bits 31:23,
# length 2n-1 in bits 7:0; MI_LRI_LRM_CS_MMIO (bit 19) makes offsets engine-relative.
MI_LRI_OPCODE, MI_LRI_CS_MMIO = 0x22, 1 << 19
# Engine-relative offsets of the registers a Gen12 RCS context restores
# (i915 intel_lrc_reg.h CTX_* order: CONTEXT_CONTROL, ring, ..., PDPs).
CTX_REG_NAMES = {
    0x244: "CONTEXT_CONTROL", 0x034: "RING_HEAD", 0x030: "RING_TAIL", 0x038: "RING_START",
    0x03c: "RING_CTL", 0x168: "BB_ADDR_UDW", 0x140: "BB_ADDR", 0x110: "BB_STATE",
    0x1c0: "BB_PER_CTX_PTR", 0x1c4: "INDIRECT_CTX", 0x1c8: "INDIRECT_CTX_OFFSET",
    0x3a8: "CTX_TIMESTAMP", 0x28c: "PDP3_UDW", 0x288: "PDP3_LDW", 0x284: "PDP2_UDW",
    0x280: "PDP2_LDW", 0x27c: "PDP1_UDW", 0x278: "PDP1_LDW", 0x274: "PDP0_UDW", 0x270: "PDP0_LDW",
    0x180: "CCID", 0x2b4: "SEMA_TOKEN", 0x1b0: "POSH_LRCA", 0x5a8: "CS_CTX_TS_UDW", 0x5ac: "CS_CTX_TS",
    0x0c8: "R_PWR_CLK_STATE", 0x028: "EXCC", 0x09c: "MI_MODE", 0x0c0: "INSTPM",
}
# i915 gen12_rcs_offsets (intel_lrc.c): the register order a Gen12 RCS context
# restores in its first four LRI blocks.
I915_GEN12_RCS_ORDER = [0x244, 0x034, 0x030, 0x038, 0x03c, 0x168, 0x140, 0x110, 0x1c0, 0x1c4, 0x1c8, 0x180,
                        0x2b4, 0x3a8, 0x28c, 0x288, 0x284, 0x280, 0x27c, 0x278, 0x274, 0x270,
                        0x1b0, 0x5a8, 0x5ac, 0x0c8]
# i915 intel_sseu_make_rpcs on Gen12 (has_slice_pg only): RPCS_ENABLE |
# S_CNT_ENABLE | slices << 12, i.e. 0x80041000 for one slice (UHD 770 and TGL GT2).
RPCS_EXPECTED_1_SLICE = 0x80041000


def describe_descriptor(lo, hi):
    """Gen12 execlist context descriptor (i915 intel_lrc.h GEN8_CTX_*/GEN11_*)."""
    modes = {0: "advanced", 1: "legacy 32b", 2: "mode 2", 3: "legacy 64b"}
    flags = [n for b, n in ((0, "valid"), (1, "force-PD-restore"), (2, "force-restore"),
                            (5, "L3LLC-coherent"), (8, "privilege")) if lo >> b & 1]
    return (f"LRCA 0x{lo & 0xFFFFF000:08x}, {', '.join(flags) or 'no flags'}, addressing {modes[lo >> 3 & 3]}, "
            f"priority {lo >> 9 & 3}, sw ctx id {hi >> 5 & 0x7FF}, engine instance {hi >> 16 & 0x3F}, "
            f"class {hi >> 29 & 7}")

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
    if all(k in s for k in ("rcsSq0Lo", "lrcaPteLo", "lrcaPteHi")):
        lrca = s["rcsSq0Lo"] & 0xFFFFF000
        if s["lrcaPteLo"] == 0xFFFFFFFF:
            out.append(f"  RCS context (SQ0 0x{s['rcsSq0Lo']:08x}, LRCA 0x{lrca:08x}): PTE outside the mapped BAR.")
        elif lrca == 0:
            out.append("  RCS context: submit queue reads 0 (no descriptor latched or not readable).")
        else:
            valid, addr = s["lrcaPteLo"] & 1, pte_address(s, "lrcaPte")
            out.append(f"  RCS context (SQ0 0x{s['rcsSq0Hi']:08x}_{s['rcsSq0Lo']:08x}, LRCA GGTT 0x{lrca:08x}) "
                       f"-> PTE 0x{pte(s, 'lrcaPte'):016x} ({'valid' if valid else 'NOT valid'}, physical 0x{addr:x})"
                       + (": the context image is read from and saved to physical 0 (cf. the DMAR write at 0x0)."
                          if valid and addr == 0 else "."))
            out.append(f"    descriptor: {describe_descriptor(s['rcsSq0Lo'], s['rcsSq0Hi'])}"
                       " (i915 Gen12 submits valid, privilege, legacy 64b)")
    if "rcsCcid" in s:
        ccid = s["rcsCcid"]
        out.append(f"  RCS CCID 0x{ccid:08x}" + (f" (current context GGTT 0x{ccid & 0xFFFFF000:08x}, enabled)"
                                                  if ccid & 1 else " (no current context)"))
    if all(k in s for k in ("rcControl", "pgEnable", "rc6CtxBase")):
        out.append(f"  RC6/PG: RC_CONTROL 0x{s['rcControl']:08x}, RC_STATE 0x{s['rcState']:08x}, PG_ENABLE "
                   f"0x{s['pgEnable']:08x}, RC6_LOCATION 0x{s['rc6Location']:08x}, RC6_CTX_BASE 0x{s['rc6CtxBase']:08x}"
                   + (" (RC6 off)" if not s["rcControl"] else " (RC6 ON: the GPU saves its power context at RC6_CTX_BASE)"))
    for eng, name in (("pocs", "POCS (POSH)"), ("ccs", "CCS (compute)")):
        if eng + "Reset" in s:
            first, last = samples[0][2].get(eng + "ExecStatus", 0), s[eng + "ExecStatus"]
            out.append(f"  {name} RESET_CTL {reset(s[eng + 'Reset'])}, EXECLIST_STATUS 0x{first:08x} -> 0x{last:08x}"
                       + (f", MI_MODE {mimode(s[eng + 'MiMode'])}" if eng + "MiMode" in s else "")
                       + (f", HWS_PGA 0x{s[eng + 'HwsPga']:08x}" if eng + "HwsPga" in s else "")
                       + (f", CONTEXT_CONTROL 0x{s[eng + 'CtxCtl']:08x}" if eng + "CtxCtl" in s else ""))
            if s[eng + "Reset"] >> 2 & 1:
                out.append(f"    -> {name} is in CATASTROPHIC error: this engine faulted, not only RCS.")
    if "rcuMode" in s:
        out.append(f"  GEN12_RCU_MODE 0x{s['rcuMode']:08x} (CCS mode {'ON' if s['rcuMode'] & 1 else 'off'}; "
                   "i915 never enables it on Gen12 LP).")
    if "rcsGfxMode" in s:
        out.append(f"  RCS GFX_MODE 0x{s['rcsGfxMode']:08x} (bit 3 DISABLE_LEGACY_MODE "
                   f"{'set' if s['rcsGfxMode'] & 8 else 'clear, as Apple initModeRegisters writes it'}).")
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


def context_dumps(text):
    """{lrca: {page: {"pte": int or None, "words": {index: value}}}} from ReimsGT ctx lines."""
    out = {}
    for m in CTX_PTE.finditer(text):
        page = out.setdefault(int(m.group(1), 16), {}).setdefault(int(m.group(2)), {"pte": None, "words": {}})
        page["pte"] = int(m.group(3), 16)
    for m in CTX_WORDS.finditer(text):
        page = out.setdefault(int(m.group(1), 16), {}).setdefault(int(m.group(2)), {"pte": None, "words": {}})
        for i, v in enumerate(m.group(4).split()):
            page["words"][int(m.group(3)) + i] = int(v, 16)
    for m in CTX_NOTE.finditer(text):
        page = out.setdefault(int(m.group(1), 16), {}).setdefault(int(m.group(2)), {"pte": None, "words": {}})
        page["note"] = m.group(3)
    return out


def decode_lri(words):
    """[(offset, value)] from the MI_LOAD_REGISTER_IMM blocks in a dword list."""
    regs, i = [], 0
    while i < len(words):
        w = words[i]
        if w >> 23 == MI_LRI_OPCODE:
            n = ((w & 0xFF) + 1) // 2
            for k in range(n):
                if i + 2 + 2 * k < len(words):
                    regs.append((words[i + 1 + 2 * k], words[i + 2 + 2 * k]))
            i += 1 + 2 * n
        else:
            i += 1
    return regs


GTT_LINE = re.compile(r"ReimsGT gtt ([0-9a-f]{8})((?: [0-9a-f]{16})+)")
# MI opcodes (bits 28:23 with client bits 31:29 = 0) and PIPE_CONTROL, from
# i915 intel_gpu_commands.h; enough to read what Apple queued in the ring.
MI_NAMES = {0x00: "MI_NOOP", 0x02: "MI_USER_INTERRUPT", 0x05: "MI_ARB_CHECK", 0x08: "MI_ARB_ON_OFF",
            0x0A: "MI_BATCH_BUFFER_END", 0x1C: "MI_SEMAPHORE_WAIT", 0x20: "MI_STORE_DATA_IMM",
            0x22: "MI_LOAD_REGISTER_IMM", 0x24: "MI_STORE_REGISTER_MEM", 0x26: "MI_FLUSH_DW",
            0x29: "MI_LOAD_REGISTER_MEM", 0x31: "MI_BATCH_BUFFER_START"}


def command_name(dw):
    if dw >> 29 == 0:
        return MI_NAMES.get((dw >> 23) & 0x3F, f"MI_0x{(dw >> 23) & 0x3F:02x}")
    if dw >> 16 == 0x7A00:
        return "PIPE_CONTROL"
    return f"cmd 0x{dw:08x}"


def ring_commands(words):
    """[(dword index, name)] walking the ring from dword 0 (MI length = low 8 bits + 2)."""
    out, i = [], 0
    while i < len(words):
        dw = words[i]
        name = command_name(dw)
        out.append((i, name))
        if dw >> 29 == 0:
            op = (dw >> 23) & 0x3F
            i += 1 if op < 0x10 else (dw & 0xFF) + 2
        elif dw >> 16 == 0x7A00:
            i += (dw & 0xFF) + 2
        else:
            i += 1
    return out


def gtt_window(text):
    """{gtt address: pte} from ReimsGT gtt lines."""
    out = {}
    for m in GTT_LINE.finditer(text):
        base = int(m.group(1), 16)
        for i, v in enumerate(m.group(2).split()):
            out[base + i * 4096] = int(v, 16)
    return out


GTT_BAD = re.compile(r"ReimsGT gttbad ([0-9a-f]{8}) ([0-9a-f]{16})")
GTT_SCAN = re.compile(r"ReimsGT gttscan ([0-9a-f]{8}) (\d+) valid (\d+) invalid (\d+) zero (\d+)")


def gtt_scan_summary(text):
    out = []
    for m in GTT_SCAN.finditer(text):
        start, pages = int(m.group(1), 16), int(m.group(2))
        out.append(f"GGTT scan 0x{start:08x}-0x{start + pages * 4096 - 1:08x}: {m.group(3)} valid, "
                   f"{m.group(4)} not valid, {m.group(5)} valid at physical 0")
    bad = [(int(a, 16), int(p, 16)) for a, p in GTT_BAD.findall(text)]
    zero = [a for a, p in bad if p & 1]
    if zero:
        out.append("  valid entries at physical 0: " + ", ".join(f"0x{a:08x}" for a in zero[:16]))
    invalid = [a for a, p in bad if not p & 1]
    if invalid:
        out.append("  first entries not valid: " + ", ".join(f"0x{a:08x}" for a in invalid[:16]))
    return out


def gtt_summary(window):
    if not window:
        return []
    valid = {a: p for a, p in window.items() if p & 1}
    zero = [a for a, p in valid.items() if not p & 0x7FFFFFF000]
    out = [f"GGTT window 0x{min(window):08x}-0x{max(window) + 4095:08x}: {len(valid)} of {len(window)} entries valid"]
    if zero:
        out.append(f"  {len(zero)} valid entr{'y' if len(zero) == 1 else 'ies'} pointing at physical 0: "
                   + ", ".join(f"0x{a:08x}" for a in sorted(zero)[:16]))
    runs, start, prev = [], None, None
    for a in sorted(valid):
        phys = valid[a] & 0x7FFFFFF000
        if start is None or a != prev[0] + 4096 or phys != prev[1] + 4096:
            if start is not None:
                runs.append((start, prev[0], prev[1]))
            start = (a, phys)
        prev = (a, phys)
    if start is not None:
        runs.append((start, prev[0], prev[1]))
    for (a0, p0), a1, p1 in runs[:24]:
        out.append(f"  GGTT 0x{a0:08x}-0x{a1 + 4095:08x} -> physical 0x{p0:x}" + ("" if a0 == a1 else f" .. 0x{p1:x}"))
    if len(runs) > 24:
        out.append(f"  ... {len(runs) - 24} more runs")
    return out


def context_summary(dumps):
    out = []
    for lrca, pages in sorted(dumps.items()):
        out.append(f"Submitted context LRCA GGTT 0x{lrca:08x}:")
        for pn in sorted(pages):
            p = pages[pn]
            words = [p["words"].get(i, 0) for i in range(max(p["words"], default=-1) + 1)]
            pte = p["pte"]
            if p.get("note") == "PPGTT root is 0":
                out.append(f"  page {pn}: PDP0 in the restore list is 0: page walks start at physical 0.")
                continue
            if p.get("note"):
                out.append(f"  page {pn}: {p['note']}")
                continue
            if pn == 4:  # PPGTT root: a physical page, the header carries its address
                where = f"physical 0x{pte:x} (PDP0 from the restore list)" if pte is not None else "not logged"
            else:
                where = (f"PTE 0x{pte:016x} ({'valid' if pte & 1 else 'NOT valid'}, physical 0x{pte & 0x7FFFFFF000:x})"
                         if pte is not None else "PTE not logged")
            kind = {0: "per-process status page", 1: "register state", 2: "ring buffer (from RING_START)",
                    3: "POSH context register state (POSH_LRCA)", 4: "PPGTT root (PML4)",
                    5: "second submitted context (SQ1) register state"}.get(pn, f"page {pn}")
            out.append(f"  page {pn} ({kind}): {where}; {len(words)} dwords"
                       + (", ALL ZERO" if words and not any(words) else ""))
            if pn != 4 and pte is not None and pte & 1 and not pte & 0x7FFFFFF000:
                out.append("    -> this page is guest-physical 0: the engine reads/writes the context at address 0.")
            if pn == 2 and words:
                cmds = ring_commands(words)
                out.append("    commands: " + ", ".join(f"@{i} {n}" for i, n in cmds[:24])
                           + (" ..." if len(cmds) > 24 else ""))
                continue
            if pn == 4 and words:
                entries = [(words[i + 1] << 32 | words[i]) for i in range(0, len(words) - 1, 2)]
                present = [(i, e) for i, e in enumerate(entries) if e & 1]
                at_zero = [i for i, e in present if not e & 0x7FFFFFF000]
                out.append(f"    {len(present)} of {len(entries)} root entries present"
                           + (", first " + ", ".join(f"[{i}] 0x{e & 0x7FFFFFF000:x}" for i, e in present[:4])
                              if present else ": EMPTY root, every PPGTT address is unmapped"))
                if at_zero:
                    out.append(f"    -> present root entries pointing at physical 0: {at_zero[:8]}: page walks go to 0.")
                continue
            if pn not in (1, 3, 5) or not words:
                continue
            regs = decode_lri(words)
            if not regs:
                out.append("    no MI_LOAD_REGISTER_IMM found: not a context register-state page.")
                continue
            for off, val in regs:
                name = CTX_REG_NAMES.get(off & 0xFFF, "")
                out.append(f"    LRI 0x{off:05x} {name:<18} = 0x{val:08x}")
            named = {CTX_REG_NAMES.get(off & 0xFFF): val for off, val in regs}
            order = [off & 0xFFF for off, _ in regs][:len(I915_GEN12_RCS_ORDER)]
            if pn == 3:
                pass  # POSH context: its own layout, not compared with i915's RCS list
            elif order == I915_GEN12_RCS_ORDER[:len(order)]:
                out.append(f"    -> restore list matches i915's Gen12 RCS layout ({len(order)} registers checked).")
            else:
                first = next(i for i, (a, b) in enumerate(zip(order, I915_GEN12_RCS_ORDER)) if a != b) \
                    if any(a != b for a, b in zip(order, I915_GEN12_RCS_ORDER)) else len(order)
                out.append(f"    -> restore list DIFFERS from i915's Gen12 RCS layout at entry {first}: "
                           + ", ".join(f"0x{o:03x}" for o in order[first:first + 4]) + " vs i915 "
                           + ", ".join(f"0x{o:03x}" for o in I915_GEN12_RCS_ORDER[first:first + 4]))
            if "R_PWR_CLK_STATE" in named:
                rpcs = named["R_PWR_CLK_STATE"]
                out.append(f"    -> R_PWR_CLK_STATE 0x{rpcs:08x}: slices {rpcs >> 12 & 0x3F}, "
                           f"subslice count {'on ' + str(rpcs >> 8 & 7) if rpcs >> 11 & 1 else 'off'}, "
                           f"EU min/max {rpcs & 0xF}/{rpcs >> 4 & 0xF}"
                           + (" (= i915 for 1 slice)" if rpcs == RPCS_EXPECTED_1_SLICE else
                              f" (i915 would program 0x{RPCS_EXPECTED_1_SLICE:08x} for 1 slice)"))
            if "RING_CTL" in named and not named["RING_CTL"] & RING_VALID:
                out.append("    -> RING_CTL in the image has no valid bit: restoring this context leaves no ring "
                           "(matches ring registers staying 0 and CAT_ERROR).")
            if "RING_START" in named:
                out.append(f"    -> ring buffer at GGTT 0x{named['RING_START'] & 0xFFFFF000:08x}.")
            if "PDP0_LDW" in named:
                root = (named.get("PDP0_UDW", 0) << 32 | named["PDP0_LDW"]) & 0x7FFFFFF000
                out.append(f"    -> PPGTT root (PDP0) physical 0x{root:x}"
                           + (": page walks start at physical 0." if root == 0 else "."))
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
                    + [""] + hang_verdict(samples, msi) + [""] + context_summary(context_dumps(text)) + gtt_summary(gtt_window(text)) + gtt_scan_summary(text)))


if __name__ == "__main__":
    main(sys.argv)
