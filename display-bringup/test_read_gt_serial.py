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

    def test_empty_log(self):
        self.assertIn("No ReimsGT samples", R.summary([])[0])


if __name__ == "__main__":
    unittest.main()
