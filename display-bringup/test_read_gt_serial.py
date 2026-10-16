#!/usr/bin/env python3
"""Host test of read_gt_serial.py on synthetic serial logs."""
import pathlib
import re
import sys
import unittest

BASE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(BASE))
import read_gt_serial as R  # noqa: E402

N = len(R.DEFAULT_NAMES)


def line(seq, ms, **values):
    v = {n: 0 for n in R.DEFAULT_NAMES}
    v.update(values)
    return f"ReimsGT {seq} {ms} " + " ".join(f"{v[n]:08x}" for n in R.DEFAULT_NAMES)


HEADER = "ReimsGT seq ms " + " ".join(R.DEFAULT_NAMES)
LIT = dict(transconfA=0xC0000000, surfLiveA=0x01000000, fbPteLo=0x12345001, fbPteHi=0x1)
AWAKE = dict(fwAckGT=0x2, fwAckRender=0x2)
RCS_ON = dict(rcsCtl=0x7001)  # valid, 8 pages (32 KiB, the profile's RingSizeKB)


class Decoder(unittest.TestCase):
    def test_columns_match_the_kext(self):
        source = (BASE / "gt_sampler.hpp").read_text()
        self.assertEqual(re.findall(r'\{[^{},]+,"(\w+)"(?:,\d)?\}', source), R.DEFAULT_NAMES)

    def test_parse_skips_noise_and_bad_lines(self):
        text = "\n".join([
            "OpenCore: booting", HEADER, "kernel: [ 12.3] ReimsGT 0 0 " + " ".join(["00000000"] * N),
            "ReimsGT 1 250 0000", line(2, 500, **LIT), "garbage",
        ])
        names, samples = R.parse(text)
        self.assertEqual(names, R.DEFAULT_NAMES)
        self.assertEqual([s[0] for s in samples], [0, 2])
        self.assertEqual(samples[1][2]["surfLiveA"], 0x01000000)

    def test_matches_the_cpp_formatter(self):
        # test_gt_sampler.cpp: seq 7, 1234 ms, v0=0x00010001, v1=0x11111111, v2=0x8000f0f4.
        values = [0x11111111 * (i % 16) for i in range(N)]
        values[0], values[2] = 0x00010001, 0x8000F0F4
        text = "ReimsGT 7 1234 " + " ".join(f"{v:08x}" for v in values)
        _, samples = R.parse(text)
        self.assertEqual(samples[0][2]["gucStatus"], 0x8000F0F4)

    def test_decoders(self):
        self.assertIn("ukernel=0xf0", R.guc(0x8000F0F4))
        self.assertIn("auth=good", R.guc(0x8000F0F4))
        self.assertIn("auth=BAD", R.guc(0x40000000))
        self.assertEqual(R.reset(0b101), "request,CATASTROPHIC")
        self.assertEqual(R.mimode(1 << 9), "idle")
        s = {"rcsHead": 0x40, "rcsTail": 0x80, "rcsCtl": 0x7001}
        self.assertIn("64 bytes queued", R.ring(s, "rcs"))
        self.assertEqual(R.ring({"rcsHead": 0, "rcsTail": 0, "rcsCtl": 0}, "rcs"), "off")

    def test_stalled_render_engine(self):
        lines = [HEADER, line(0, 0, **LIT)]
        busy = dict(LIT, **AWAKE, **RCS_ON, rcsHead=0x40, rcsTail=0x100, rcsActhd=0x1234,
                    gucStatus=0x8000F000)
        lines += [line(4, 1000, **busy), line(24, 6000, **busy), line(44, 11000, **busy)]
        out = "\n".join(R.summary(R.parse("\n".join(lines))[1]))
        self.assertIn("RCS has queued work and ACTHD is not moving", out)
        self.assertIn("forcewake: acknowledged", out)
        self.assertIn("last register change at 1000 ms", out)

    def test_forcewake_never_acked_and_display_lost(self):
        lines = [line(0, 0, **LIT), line(20, 5000, fwAckGT=0x1), line(40, 10000, fwAckGT=0x1)]
        out = "\n".join(R.summary(R.parse("\n".join(lines))[1]))
        self.assertIn("not acknowledged on GT and/or Render", out)
        self.assertIn("Pipe A was active at the start and is not now", out)
        self.assertIn("Plane 1A moved from 0x01000000 to 0x00000000", out)

    def test_silent_sampler_and_errors(self):
        lines = [line(0, 0, **LIT), line(20, 5000, **LIT), line(21, 31000, **LIT, errorGen6=0x8, rcsReset=0x4)]
        out = "\n".join(R.summary(R.parse("\n".join(lines))[1]))
        self.assertIn("went silent for 26.0 s after 5000 ms and its timer ran only 1 of ~104 ticks", out)
        self.assertIn("GT error registers nonzero: ERROR_GEN6 0x00000008", out)
        self.assertIn("catastrophic error flagged", out)

    def test_gap_with_timer_running_is_lost_output(self):
        lines = [line(0, 0, **LIT), line(120, 30000, **LIT)]
        out = "\n".join(R.summary(R.parse("\n".join(lines))[1]))
        self.assertIn("timer kept running (120 ticks): lines were lost", out)

    def test_unconsumed_csb_entries(self):
        self.assertEqual(R.csb(0x0B03), (11, 3, 4))
        self.assertEqual(R.csb(0x0F0F)[2], None)
        stuck = dict(LIT, **AWAKE, **RCS_ON, rcsCsb=0x0001)
        lines = [line(0, 0, **LIT), line(4, 1000, **stuck), line(24, 6000, **stuck)]
        out = "\n".join(R.summary(R.parse("\n".join(lines))[1]))
        self.assertIn("1 unread entries that are not being consumed", out)

    def test_framebuffer_pte_reused(self):
        reused = dict(LIT, fbPteLo=0x0BAD0001, fbPteHi=0)
        lines = [line(0, 0, **LIT), line(40, 10000, **reused)]
        out = "\n".join(R.summary(R.parse("\n".join(lines))[1]))
        self.assertIn("changed from 0x0000000112345001 to 0x000000000bad0001", out)
        self.assertIn("valid, address bits 0x0bad0000", R.describe("fbPteLo", 0x0BAD0001))

    def test_timeline_lists_changes(self):
        samples = R.parse("\n".join([line(0, 0, **LIT), line(1, 250, **LIT, gucStatus=0x200)]))[1]
        text = "\n".join(R.timeline(samples))
        self.assertIn("1 changed", text)
        self.assertIn("gucStatus: 0x00000000", text)
        self.assertIn("bootrom=0x00 ukernel=0x02", text)

    def test_forcewake_any_requester_bit(self):
        held = dict(LIT, fwAckGT=0x4, fwAckRender=0x6)  # hold probe bit 2, Apple bit 1
        out = "\n".join(R.summary(R.parse("\n".join([line(0, 0, **held), line(1, 250, **held)]))[1]))
        self.assertIn("forcewake: acknowledged (GT awake; GT requester bits 0x4)", out)
        self.assertIn("acked, requester bits 0x6", R.describe("fwAckRender", 6))

    def test_pending_gt_interrupt(self):
        irq = dict(LIT, **AWAKE, gtIntrDw0=0x1, gfxMstrIrq=0x80000001)
        lines = [line(0, 0, **LIT), line(4, 1000, **irq), line(24, 6000, **irq), line(44, 11000, **irq)]
        out = "\n".join(R.summary(R.parse("\n".join(lines))[1]))
        self.assertIn("GT interrupt pending and not serviced over the last 3 samples", out)
        self.assertIn("master enabled, pending: GT DW0", out)
        quiet = "\n".join(R.summary(R.parse("\n".join([line(0, 0, **LIT), line(1, 250, **LIT)]))[1]))
        self.assertNotIn("GT interrupt pending", quiet)

    def test_stall_lines(self):
        text = "\n".join([line(0, 0, **LIT),
                          "ReimsGT stall seq 12 ms 34567 reading rcsHead 0x002034 for 2250 ms",
                          "ReimsGT stall seq 12 ms 39567 reading rcsHead 0x002034 for 7250 ms"])
        found = R.stalls(text)
        self.assertEqual(found[-1], (12, 39567, "rcsHead", 0x2034, 7250))
        self.assertIn("STUCK in a register read: rcsHead (0x002034)", R.stall_summary(found)[0])
        starved = R.stalls("ReimsGT stall seq 3 ms 900 starved for 3000 ms")
        self.assertIn("STARVED", R.stall_summary(starved)[0])
        self.assertEqual(R.stall_summary([]), [])
        self.assertEqual(len(R.parse(text)[1]), 1)  # stall lines are not samples

    def verdict(self, states, msi=None):
        lines = [line(i * 20, i * 5000, **st) for i, st in enumerate(states)]
        return "\n".join(R.hang_verdict(R.parse("\n".join(lines))[1], msi))

    # Interrupt setup Apple writes standalone (enableInterrupts, master bit).
    IRQ_ON = dict(gfxMstrIrq=0x80000000, rcsBcsIntrEn=0x01100110)

    def test_verdict_never_executed(self):
        stuck = dict(LIT, **AWAKE, **RCS_ON, **self.IRQ_ON, rcsHead=0x40, rcsTail=0x100, rcsActhd=0x1040)
        out = self.verdict([stuck, stuck, stuck], msi=False)
        self.assertIn("NEVER moved", out)
        self.assertIn("the engine never executed the stamps", out)
        self.assertIn("host vfio-msi count during the hang: did not move", out)

    def test_verdict_no_context(self):
        idle = dict(LIT, **AWAKE, **self.IRQ_ON)
        self.assertIn("No context is running on RCS", self.verdict([idle, idle]))

    def test_verdict_executed_master_masked(self):
        a = dict(LIT, **AWAKE, **RCS_ON, rcsBcsIntrEn=0x01100110, rcsHead=0x40, rcsTail=0x100, rcsActhd=0x1040)
        b = dict(a, rcsHead=0x100, rcsActhd=0x1100, gtIntrDw0=0x1)  # drained, pending, master clear
        out = self.verdict([a, b, b])
        self.assertIn("moved during the capture", out)
        self.assertIn("master IRQ (0x190010 bit 31): CLEAR", out)
        self.assertIn("masked at the master", out)

    def test_verdict_pending_but_no_msi(self):
        a = dict(LIT, **AWAKE, **RCS_ON, **self.IRQ_ON, rcsHead=0x40, rcsTail=0x100, rcsActhd=0x1040)
        b = dict(a, rcsHead=0x100, rcsActhd=0x1100, gtIntrDw0=0x1)
        self.assertIn("the interrupt is not reaching the MSI", self.verdict([a, b, b], msi=False))

    def test_verdict_masked_at_engine(self):
        a = dict(LIT, **AWAKE, **RCS_ON, gfxMstrIrq=0x80000000, rcsBcsIntrEn=0x01100110,
                 rcsHead=0x40, rcsTail=0x100, rcsActhd=0x1040)
        b = dict(a, rcsHead=0x100, rcsActhd=0x1100, rcsImr=0xffffffff)
        out = self.verdict([a, b, b])
        self.assertIn("RCS IMR masks: USER, PIPECTL_NOTIFY, CONTEXT_SWITCH", out)
        self.assertIn("masked at the engine", out)

    def test_verdict_executed_nothing_pending(self):
        a = dict(LIT, **AWAKE, **RCS_ON, **self.IRQ_ON, rcsHead=0x40, rcsTail=0x100, rcsActhd=0x1040)
        b = dict(a, rcsHead=0x100, rcsActhd=0x1100)
        out = self.verdict([a, b, b], msi=False)
        self.assertIn("RCS interrupt enable: PIPECTL_NOTIFY, CONTEXT_SWITCH", out)
        self.assertIn("did not ask for an interrupt", out)
        self.assertIn("so not serviced", out)

    def test_main_accepts_msi_flag(self):
        import contextlib
        import io
        import tempfile
        st = dict(LIT, **AWAKE, **RCS_ON, **self.IRQ_ON, rcsHead=0x40, rcsTail=0x100, rcsActhd=0x1040)
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write("\n".join([line(0, 0, **st), line(20, 5000, **st)]))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            R.main(["x", f.name, "--msi", "flat"])
        self.assertIn("host vfio-msi count during the hang: did not move", buf.getvalue())
        pathlib.Path(f.name).unlink()

    def test_parts_are_reassembled(self):
        v = {n: i for i, n in enumerate(R.DEFAULT_NAMES)}
        names = R.DEFAULT_NAMES
        lines = []
        for first in range(0, len(names), 10):
            lines.append("ReimsGT seq ms @%d %s" % (first, " ".join(names[first:first + 10])))
        for seq, ms in ((1, 250), (2, 500)):
            for first in range(0, len(names), 10):
                lines.append(f"ReimsGT {seq} {ms} @{first} " + " ".join(f"{v[n]:08x}" for n in names[first:first + 10]))
        got_names, samples = R.parse("\n".join(lines))
        self.assertEqual(got_names, names)
        self.assertEqual([(s[0], s[1]) for s in samples], [(1, 250), (2, 500)])
        self.assertEqual(samples[0][2], v)
        self.assertEqual(R.truncated(samples), [])

    def test_truncated_legacy_line_keeps_full_columns(self):
        full = line(3, 503, **LIT)
        cut = full[:R.CONSOLE_LINE]
        _, samples = R.parse(cut)
        self.assertEqual(len(samples), 1)
        kept = samples[0][2]
        self.assertEqual(len(kept), (R.CONSOLE_LINE - len("ReimsGT 3 503")) // 9)
        self.assertEqual(list(kept), R.DEFAULT_NAMES[:len(kept)])
        self.assertIn("fbPteLo", R.truncated(samples))
        # Raw capture: the cut line lost its newline, so the next sample (and
        # other console output) follows on the same line.
        raw = "kernel noise\n" + cut + line(23, 5524, **LIT)[:R.CONSOLE_LINE] + "\n[IGPU] something\n"
        both = R.parse(raw)[1]
        self.assertEqual([(s[0], s[1]) for s in both], [(3, 503), (23, 5524)])
        # A short garbled line (not at the limit) is still rejected.
        self.assertEqual(R.parse(full[:120])[1], [])
        out = "\n".join(R.summary(samples + samples))
        self.assertIn("columns missing from some samples", out)

    def test_verdict_catastrophic_error_never_executed(self):
        # 2026-10-15 window, decoded: interrupts armed, no context ever loaded,
        # CAT_ERROR with periodic reset requests that never get READY_TO_RESET.
        before = dict(LIT, fwAckGT=0x4, fwAckRender=0x4, rcsCsb=0x7007, rcsMiMode=0x200, errorGen6=0x37)
        armed = dict(LIT, fwAckGT=0x2, fwAckRender=0x2, gfxMstrIrq=0x80000000, rcsBcsIntrEn=0x01100110,
                     rcsExecStatus=0x40003018, rcsReset=0x4, errorGen6=0x7b)
        states = [before, armed, dict(armed, rcsReset=0x5), armed, dict(armed, rcsReset=0x5), armed]
        out = self.verdict(states, msi=False)
        self.assertIn("RING_RESET_CTL: CATASTROPHIC at the end; 2 reset request(s) seen, READY_TO_RESET never seen", out)
        self.assertIn("never advanced (no context-switch event)", out)
        self.assertIn("HAS#396813", out)
        self.assertIn("=> NEVER EXECUTED, not an interrupt problem", out)
        summary = "\n".join(R.summary(R.parse("\n".join(
            line(i * 20, i * 5000, **st) for i, st in enumerate(states)))[1]))
        self.assertIn("forcewake bit 2 was cleared", summary)

    def test_status_page_pte(self):
        to_zero = dict(LIT, **AWAKE, rcsHwsPga=0x00123000, hwsPteLo=0x00000003, hwsPteHi=0,
                       ggttPte0Lo=0x12345001, ggttPte0Hi=0)
        out = "\n".join(R.summary(R.parse("\n".join([line(0, 0, **to_zero), line(1, 250, **to_zero)]))[1]))
        self.assertIn("RCS status page: GGTT 0x00123000 -> PTE 0x0000000000000003 (valid, physical 0x0)", out)
        self.assertIn("go to physical 0", out)
        self.assertIn("GGTT entry 0: 0x0000000012345001 (valid, physical 0x12345000)", out)
        ok = dict(to_zero, hwsPteLo=0x7A001003, hwsPteHi=0x1)
        out = "\n".join(R.summary(R.parse("\n".join([line(0, 0, **ok), line(1, 250, **ok)]))[1]))
        self.assertIn("physical 0x17a001000).", out)
        self.assertIn("outside the mapped BAR", R.describe("hwsPteLo", 0xFFFFFFFF))

    def test_submitted_context_pte(self):
        st = dict(LIT, **AWAKE, rcsSq0Lo=0x40008019, rcsSq0Hi=0x00000020, lrcaPteLo=0x00000003,
                  lrcaPteHi=0, rcsGfxMode=0)
        out = "\n".join(R.summary(R.parse("\n".join([line(0, 0, **st), line(1, 250, **st)]))[1]))
        self.assertIn("LRCA GGTT 0x40008000) -> PTE 0x0000000000000003 (valid, physical 0x0)", out)
        self.assertIn("context image is read from and saved to physical 0", out)
        self.assertIn("GFX_MODE 0x00000000 (bit 3 DISABLE_LEGACY_MODE clear", out)
        ok = dict(st, lrcaPteLo=0x4b75808b, lrcaPteHi=1)
        out = "\n".join(R.summary(R.parse("\n".join([line(0, 0, **ok), line(1, 250, **ok)]))[1]))
        self.assertIn("physical 0x14b758000).", out)
        idle = dict(st, rcsSq0Lo=0, rcsSq0Hi=0)
        out = "\n".join(R.summary(R.parse("\n".join([line(0, 0, **idle), line(1, 250, **idle)]))[1]))
        self.assertIn("submit queue reads 0", out)

    def test_context_dump_lri(self):
        # Page 1 as i915 lays out a Gen12 RCS context: NOOP, LRI(13) with
        # CONTEXT_CONTROL then the ring registers (engine-relative, CS_MMIO).
        lri = (0x22 << 23) | (1 << 19) | (2 * 13 - 1)
        page1 = [0, lri, 0x244, 0x00090009, 0x034, 0, 0x030, 0, 0x038, 0x40010000, 0x03c, 0x00007000,
                 0x168, 0, 0x140, 0, 0x110, 0, 0x1c0, 0, 0x1c4, 0, 0x1c8, 0, 0x3a8, 0, 0x28c, 0]
        page1 += [0] * (64 - len(page1))
        lrca, lines = 0x40008000, []
        lines.append(f"ReimsGT ctx {lrca:08x} p0 pte 000000014b75808b")
        lines += [f"ReimsGT ctx {lrca:08x} p0 @{i} " + " ".join("00000000" for _ in range(8)) for i in range(0, 64, 8)]
        lines.append(f"ReimsGT ctx {lrca:08x} p1 pte 0000000000000003")
        lines += [f"ReimsGT ctx {lrca:08x} p1 @{i} " + " ".join(f"{w:08x}" for w in page1[i:i + 8])
                  for i in range(0, 64, 8)]
        dumps = R.context_dumps("\n".join(lines))
        self.assertEqual(sorted(dumps[lrca]), [0, 1])
        out = "\n".join(R.context_summary(dumps))
        self.assertIn("page 0 (per-process status page): PTE 0x000000014b75808b (valid, physical 0x14b758000); "
                      "64 dwords, ALL ZERO", out)
        self.assertIn("page 1 (register state): PTE 0x0000000000000003 (valid, physical 0x0)", out)
        self.assertIn("this page is guest-physical 0", out)
        self.assertIn("LRI 0x00244 CONTEXT_CONTROL    = 0x00090009", out)
        self.assertIn("LRI 0x0003c RING_CTL           = 0x00007000", out)
        self.assertIn("RING_CTL in the image has no valid bit", out)
        self.assertIn("ring buffer at GGTT 0x40010000", out)
        # This fixture omits CCID/SEMA_TOKEN (0x180/0x2b4): the layout check must notice.
        self.assertIn("DIFFERS from i915's Gen12 RCS layout at entry 11: 0x3a8, 0x28c vs i915 0x180, 0x2b4", out)
        self.assertEqual(R.decode_lri([0, 0, 0]), [])
        self.assertIn("no MI_LOAD_REGISTER_IMM found", "\n".join(R.context_summary(
            {1: {1: {"pte": 1, "words": {i: 0 for i in range(64)}}}})))

    def test_full_i915_layout_rpcs_and_descriptor(self):
        lri = lambda n, posted=True: (0x22 << 23) | (1 << 19) | (1 << 12 if posted else 0) | (2 * n - 1)
        regs1 = [0x244, 0x034, 0x030, 0x038, 0x03c, 0x168, 0x140, 0x110, 0x1c0, 0x1c4, 0x1c8, 0x180, 0x2b4]
        regs2 = [0x3a8, 0x28c, 0x288, 0x284, 0x280, 0x27c, 0x278, 0x274, 0x270]
        page = [0, lri(13)] + sum(([r, 0] for r in regs1), []) + [0] * 5
        page += [lri(9)] + sum(([r, 0] for r in regs2), [])
        page += [lri(3), 0x1b0, 0, 0x5a8, 0, 0x5ac, 0] + [0] * 6 + [lri(1, False), 0x0c8, 0x80041000]
        self.assertLess(len(page), 96)
        regs = R.decode_lri(page)
        self.assertEqual([o for o, _ in regs], R.I915_GEN12_RCS_ORDER)
        dumps = {0x40008000: {1: {"pte": 0x14b75808b, "words": dict(enumerate(page))}}}
        out = "\n".join(R.context_summary(dumps))
        self.assertIn("matches i915's Gen12 RCS layout (26 registers checked)", out)
        self.assertIn("R_PWR_CLK_STATE 0x80041000: slices 1, subslice count off, EU min/max 0/0 (= i915 for 1 slice)",
                      out)
        bad = list(page)
        bad[bad.index(0x80041000)] = 0x80042000  # two slices on a one-slice GT
        out = "\n".join(R.context_summary({1: {1: {"pte": 1 | 0x1000, "words": dict(enumerate(bad))}}}))
        self.assertIn("slices 2", out)
        self.assertIn("i915 would program 0x80041000", out)
        swapped = list(page)
        swapped[2], swapped[4] = swapped[4], swapped[2]  # RING_HEAD before CONTEXT_CONTROL
        out = "\n".join(R.context_summary({1: {1: {"pte": 0x1001, "words": dict(enumerate(swapped))}}}))
        self.assertIn("DIFFERS from i915's Gen12 RCS layout at entry 0", out)
        d = R.describe_descriptor(0x40008119 | 3 << 3, (5 << 5) | (0 << 16))
        self.assertIn("LRCA 0x40008000, valid, privilege, addressing legacy 64b", d)
        self.assertIn("sw ctx id 5", d)

    def test_ccid_and_rc6_lines(self):
        st = dict(LIT, **AWAKE, rcsCcid=0x40008001, rcControl=0, rcState=0, pgEnable=0,
                  rc6Location=0x1, rc6CtxBase=0x7c000000)
        out = "\n".join(R.summary(R.parse("\n".join([line(0, 0, **st), line(1, 250, **st)]))[1]))
        self.assertIn("RCS CCID 0x40008001 (current context GGTT 0x40008000, enabled)", out)
        self.assertIn("RC6_CTX_BASE 0x7c000000 (RC6 off)", out)
        on = dict(st, rcControl=0x88040000, rcsCcid=0)
        out = "\n".join(R.summary(R.parse("\n".join([line(0, 0, **on), line(1, 250, **on)]))[1]))
        self.assertIn("RCS CCID 0x00000000 (no current context)", out)
        self.assertIn("RC6 ON", out)

    def test_ring_commands_and_gtt_window(self):
        pc = (0x3 << 29) | (0x3 << 27) | (0x2 << 24) | (6 - 2)       # PIPE_CONTROL len 6
        sdi = (0x20 << 23) | 2                                         # MI_STORE_DATA_IMM, 4 dwords
        ring = [pc, 0, 0, 0, 0, 0, sdi, 0x40004000, 0, 0x1234, (0x02 << 23), 0, (0x0A << 23)]
        names = R.ring_commands(ring)
        self.assertEqual([n for _, n in names][:5],
                         ["PIPE_CONTROL", "MI_STORE_DATA_IMM", "MI_USER_INTERRUPT", "MI_NOOP", "MI_BATCH_BUFFER_END"])
        self.assertEqual(names[1][0], 6)
        lines = ["ReimsGT gtt 40000000 " + " ".join(f"{p:016x}" for p in
                                                  (0x14b70008b, 0x14b70108b, 0x0, 0x3, 0x14b75708b, 0x14b75808b))]
        win = R.gtt_window("\n".join(lines))
        self.assertEqual(win[0x40003000], 0x3)
        out = "\n".join(R.gtt_summary(win))
        self.assertIn("5 of 6 entries valid", out)
        self.assertIn("GGTT 0x40003000-0x40003fff -> physical 0x0\n", out + "\n")
        self.assertIn("GGTT 0x40004000-0x40005fff -> physical 0x14b757000 .. 0x14b758000", out)
        self.assertIn("1 valid entry pointing at physical 0: 0x40003000", out)
        self.assertIn("GGTT 0x40000000-0x40001fff -> physical 0x14b700000 .. 0x14b701000", out)
        self.assertEqual(R.gtt_summary({}), [])

    def test_pocs_ccs_and_rcu_mode(self):
        st = dict(LIT, **AWAKE, pocsReset=0, pocsExecStatus=0x1, pocsMiMode=0x200, pocsHwsPga=0x40005000,
                  pocsCtxCtl=0, ccsReset=0, ccsExecStatus=0x1, rcuMode=0)
        hung = dict(st, pocsReset=0x4, pocsExecStatus=0x40003018, rcuMode=0x1)
        out = "\n".join(R.summary(R.parse("\n".join([line(0, 0, **st), line(1, 250, **hung)]))[1]))
        self.assertIn("POCS (POSH) RESET_CTL CATASTROPHIC, EXECLIST_STATUS 0x00000001 -> 0x40003018, "
                      "MI_MODE idle, HWS_PGA 0x40005000, CONTEXT_CONTROL 0x00000000", out)
        self.assertIn("-> POCS (POSH) is in CATASTROPHIC error", out)
        self.assertIn("CCS (compute) RESET_CTL none, EXECLIST_STATUS 0x00000001 -> 0x00000001", out)
        self.assertNotIn("CCS (compute) is in CATASTROPHIC", out)
        self.assertIn("GEN12_RCU_MODE 0x00000001 (CCS mode ON", out)

    def test_posh_root_sq1_and_gtt_scan(self):
        lrca = 0x402d6000
        lri = (0x22 << 23) | (1 << 19) | (2 * 2 - 1)
        posh = [0, lri, 0x2244, 0xffff0009, 0x2034, 0]
        lines = [f"ReimsGT ctx {lrca:08x} p3 pte 000000014b75908b",
                 f"ReimsGT ctx {lrca:08x} p3 @0 " + " ".join(f"{w:08x}" for w in posh + [0, 0]),
                 f"ReimsGT ctx {lrca:08x} p4 pte 0000000134f94000",
                 f"ReimsGT ctx {lrca:08x} p4 @0 " + " ".join(f"{w & 0xffffffff:08x}" for w in
                                                         [0x34f95003, 0x1, 0, 0, 0x7, 0, 0, 0]),  # [2] at 0
                 f"ReimsGT ctx {lrca:08x} p5 pte 0000000000000000",
                 "ReimsGT ctx 402d8000 p4 PPGTT root is 0",
                 "ReimsGT gttbad 402b3000 000000000000008b",
                 "ReimsGT gttbad 40600000 0000000000000000",
                 "ReimsGT gttscan 40000000 2048 valid 1500 invalid 547 zero 1"]
        text = "\n".join(lines)
        out = "\n".join(R.context_summary(R.context_dumps(text)))
        self.assertIn("page 3 (POSH context register state (POSH_LRCA)): PTE 0x000000014b75908b (valid", out)
        self.assertIn("LRI 0x02244 CONTEXT_CONTROL    = 0xffff0009", out)
        self.assertNotIn("Gen12 RCS layout", out)  # POSH has its own layout
        self.assertIn("page 4 (PPGTT root (PML4)): physical 0x134f94000 (PDP0 from the restore list)", out)
        self.assertIn("2 of 4 root entries present, first [0] 0x134f95000, [2] 0x0", out)
        self.assertIn("present root entries pointing at physical 0: [2]", out)
        self.assertIn("page 5 (second submitted context (SQ1) register state): PTE 0x0000000000000000 (NOT valid",
                      out)
        self.assertIn("page 4: PDP0 in the restore list is 0", out)
        scan = "\n".join(R.gtt_scan_summary(text))
        self.assertIn("GGTT scan 0x40000000-0x407fffff: 1500 valid, 547 not valid, 1 valid at physical 0", scan)
        self.assertIn("valid entries at physical 0: 0x402b3000", scan)
        self.assertIn("first entries not valid: 0x40600000", scan)
        self.assertEqual(R.gtt_scan_summary(""), [])

    def test_empty_log(self):
        self.assertIn("No ReimsGT samples", R.summary([])[0])


if __name__ == "__main__":
    unittest.main()
