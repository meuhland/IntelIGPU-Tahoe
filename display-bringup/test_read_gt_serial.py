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
        self.assertEqual(re.findall(r'\{0x[0-9a-f]+,"(\w+)"\}', source), R.DEFAULT_NAMES)

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

    def test_empty_log(self):
        self.assertIn("No ReimsGT samples", R.summary([])[0])


if __name__ == "__main__":
    unittest.main()
