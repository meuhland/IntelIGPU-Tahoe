#!/usr/bin/env python3
"""Host test of coldgt_check.py against a simulated device (no hardware)."""
import contextlib
import io
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import coldgt_check as C  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print(f'FAIL {what}')


class Fake:
    """Simulated a780: fuses populated or cold; FLR/GDRST can change that."""

    def __init__(self, driver='vfio-pci', fuses_hot=True, flr_colds=False, gdrst_heals=False, pm_colds=False,
                 decode=True, d3=False, users=(), device=C.DEVICE, asleep=False):
        self.drv, self.users, self.device = driver, list(users), device
        self.hot, self.flr_colds, self.gdrst_heals = fuses_hot, flr_colds, gdrst_heals
        self.pm_colds = pm_colds
        self.pm_resets = 0
        self.cmd = 0x0406 if decode else 0x0404
        self.d3 = d3
        self.asleep = asleep  # GT in RC6: fuse reads 0 unless render forcewake is held
        self.reg = {}
        self.writes = []
        self.attrs = {'power/control': 'auto', 'power/runtime_status': 'suspended' if d3 else 'active',
                      'reset_method': 'flr'}
        self.mapped = self.closed = False
        self.resets = 0

    # config space: vendor/device, status w/ cap list, cap ptr 0x40 = PM cap
    def config(self, off, size):
        cfg = {0x00: C.VENDOR, 0x02: self.device, 0x04: self.cmd, 0x06: 0x0010, 0x34: 0x40,
               0x40: 0x01, 0x41: 0x00, 0x44: 3 if self.d3 else 0}
        return cfg.get(off, 0)

    def set_config16(self, off, v):
        if off == C.PCI_COMMAND:
            self.cmd = v

    def attr(self, name):
        return self.attrs.get(name)

    def set_attr(self, name, value):
        self.attrs[name] = value
        if name == 'power/control' and value == 'on':
            self.d3 = False

    def driver(self):
        return self.drv

    def vfio_users(self):
        return self.users

    def map_bar0(self):
        self.mapped = True

    def close(self):
        self.closed = True

    def pm_reset(self):
        self.pm_resets += 1
        if self.pm_colds:
            self.hot = False

    def function_reset(self):
        self.resets += 1
        if self.flr_colds:
            self.hot = False

    def delay_us(self, us):
        pass

    def read32(self, off):
        if self.d3 or not self.cmd & C.CMD_MEMORY:
            return C.ALL_ONES
        if self.asleep and off in (0x9138, 0x913c, 0x9140) and not self.reg.get(C.FW_GT[1]):
            return 0
        if off == 0x9138:
            return 1 if self.hot else 0
        if off == 0x913c:
            return 3 if self.hot else 0
        if off == 0x9140:
            return 0xe00fa if self.hot else 0xf00ff
        return self.reg.get(off, 0)

    def write32(self, off, v):
        self.writes.append(off)
        if off in (C.FW_RENDER[0], C.FW_GT[0]):
            ack = C.FW_RENDER[1] if off == C.FW_RENDER[0] else C.FW_GT[1]
            mask, val = v >> 16, v & 0xffff
            self.reg[ack] = (self.reg.get(ack, 0) & ~mask) | (val & mask)
        elif off == C.GDRST:
            self.reg[C.GDRST] = 0  # self-clears
            if self.gdrst_heals:
                self.hot = True
        else:
            self.reg[off] = v


def run(argv, dev):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = C.main(argv, dev)
    return rc, out.getvalue(), err.getvalue()


# read under i915: allowed, no writes.
d = Fake(driver='i915')
rc, out, _ = run(['read'], d)
check(rc == 0 and not d.writes and 'populated' in out and d.closed, 'read under i915')

# any write step under i915: refused before mapping.
d = Fake(driver='i915')
rc, _, err = run(['forcewake'], d)
check(rc == 2 and 'driver is i915' in err and not d.mapped, 'forcewake refused under i915')

# VM holding the vfio device: refused.
d = Fake(users=[4242])
rc, _, err = run(['gdrst'], d)
check(rc == 2 and '4242' in err and not d.writes, 'refused while VM holds vfio')

# wrong device id: refused.
d = Fake(device=0x4680)
rc, _, err = run(['read'], d)
check(rc == 2 and '4680' in err, 'wrong device refused')

# decode off: refused without the flag; with it, enabled then restored.
d = Fake(decode=False)
rc, _, err = run(['read'], d)
check(rc == 2 and '--enable-decode' in err, 'decode off refused')
d = Fake(decode=False)
rc, out, _ = run(['read', '--enable-decode'], d)
check(rc == 0 and 'populated' in out and not d.cmd & C.CMD_MEMORY, 'decode enabled then restored')

# D3 (vfio runtime suspend): all-ones -> stops; --wake fixes it and restores power/control.
d = Fake(d3=True)
rc, out, _ = run(['all'], d)
check(rc == 0 and 'UNREADABLE' in out and 'stopped' in out and d.resets == 0, 'D3 stops before FLR')
d = Fake(d3=True)
rc, out, _ = run(['read', '--wake'], d)
check(rc == 0 and 'populated' in out and d.attrs['power/control'] == 'auto', 'wake then restore')

# The reproduction we expect: bind leaves fuses hot, FLR colds them, GDRST does not heal.
d = Fake(flr_colds=True)
rc, out, _ = run(['all', '--json'], d)
res = json.loads(out)
v = res['verdict']
check(rc == 0 and d.resets == 1, 'all runs one FLR')
check(any('forcewake read): fuses populated' in s for s in v), 'verdict: bind populated')
check(any('FLR COLDS' in s for s in v), 'verdict: FLR colds')
check(any('GDRST does not re-populate' in s for s in v), 'verdict: GDRST not the lever')
check(res['steps']['gdrst']['reset_loops'] == 2 and d.writes.count(C.GDRST) == 2, 'GDRST twice')
# forcewake released after every forcewake step: last write per req is the release.
rel = [o for o in d.writes if o in (C.FW_RENDER[0], C.FW_GT[0])]
check(len(rel) == 6 * 4, "forcewake acquire+release: forcewake 1x, flr 2x, pm 2x, gdrst 1x")

# Cold after bind, GDRST heals: option 2 verdict.
d = Fake(fuses_hot=False, gdrst_heals=True)
rc, out, _ = run(['read', 'gdrst'], d)
check('COLD' in out and 'GDRST REVIVES' in out, 'verdict: GDRST revives')

# Cold after bind, FLR... stays cold (no heal): no-change verdict.
d = Fake(fuses_hot=False)
rc, out, _ = run(['flr'], d)
check('FLR: no change (cold)' in out, 'verdict: FLR no change')

# GT asleep (RC6): raw reads 0, forcewake reads populated. Must not report cold,
# and FLR must not be credited with reviving anything.
d = Fake(asleep=True)
rc, out, _ = run(['all'], d)
check('fuses populated' in out and 'GT was asleep' in out, 'asleep: forcewake verdict wins')
check('FLR REVIVES' not in out and 'FLR: no change (populated)' in out, 'asleep: no false FLR revive')

# Cold after a D3 round trip (vfio idle suspend): verdict points at disable_idle_d3.
class D3Colds(Fake):
    def set_attr(self, name, value):
        if name == 'power/control' and value == 'on' and self.d3:
            self.hot = False
        super().set_attr(name, value)
d = D3Colds(d3=True)
rc, out, _ = run(['forcewake', '--wake'], d)
check('COLD' in out and 'disable_idle_d3' in out, 'verdict: D3 round trip colds')
d = Fake(fuses_hot=False)
rc, out, _ = run(['forcewake'], d)
check('bind itself left it cold' in out and 'disable_idle_d3' not in out, 'verdict: cold without D3')
d = Fake(flr_colds=True)
rc, out, _ = run(['flr'], d)
check('reset_method' in out, 'verdict: FLR colds -> disable reset')

# PM reset (D3hot->D0) colds the GT, FLR does not: the VM signature after pm.
d = Fake(pm_colds=True)
rc, out, _ = run(['all'], d)
check(d.resets == 1 and d.pm_resets == 1, 'all runs one FLR and one PM reset')
check('FLR: no change (populated)' in out, 'pm case: FLR no change')
check('PM reset (D3hot->D0) COLDS the GT (cold)' in out, 'pm case: PM reset colds, cold signature')
check('COLD (slice/DSS zero, media live)' in out, 'pm case: media-live zero tagged cold')

# Asleep (all zero incl. media) is tagged differently from cold.
check(C.classify({'slice': 0, 'dss': 0, 'eu_disable': 0, 'media': 0}) == 'asleep', 'classify asleep')
check(C.classify({'slice': 0, 'dss': 0, 'eu_disable': 0, 'media': 0xf00ff}) == 'cold', 'classify cold')

# Apple's requester bit: --fw-bit 1 requests and acks bit 1, still wakes the GT.
d = Fake(asleep=True)
rc, out, _ = run(['forcewake', '--fw-bit', '1'], d)
check('acked: render=True gt=True' in out and 'fuses populated' in out, 'fw-bit 1 wakes and acks')
check(d.reg.get(C.FW_GT[1]) == 0, 'fw-bit 1 released')
# Asleep raw read carries the VM signature label.
check('the VM signature' in run(['read'], Fake(asleep=True))[1], 'asleep tagged as VM signature')

# engines: which command streamers answer (POCS question, 2026-10-16).
class Engines(Fake):
    """Live engines read MI_MODE idle 0x200 and a nonzero INSTDONE; others read 0."""

    def __init__(self, live=('rcs', 'bcs', 'vcs0', 'vcs2', 'vecs0', 'pocs'), debugfs=True, **kw):
        super().__init__(**kw)
        self.live = {base for n, base, _ in C.ENGINES if n in live}
        self.debugfs, self.held = debugfs, 0

    def user_forcewake_path(self):
        return '/sys/kernel/debug/dri/0/i915_forcewake_user' if self.debugfs else None

    def hold_user_forcewake(self):
        dev = self
        dev.held += 1

        class F:
            def close(self):
                dev.held -= 1
        return F()

    def read32(self, off):
        for base in self.live:
            if off == base + 0x9c:
                return 0x200
            if off == base + 0x6c:
                return 0xfffffffe
        return super().read32(off)


# Under i915: read-only (no register writes), forcewake held via debugfs and released.
d = Engines(driver='i915')
rc, out, _ = run(['engines'], d)
check(rc == 0 and not d.writes and d.held == 0, 'engines under i915: no writes, debugfs released')
check('i915_forcewake_user' in out and 'POCS answers like a live engine' in out, 'engines: POCS live verdict')
check('engines answering: rcs, pocs, bcs, vcs0, vcs2, vecs0' in out, 'engines: answering list')
# POCS absent: the prime-suspect verdict names the absent baseline.
d = Engines(driver='i915', live=('rcs', 'bcs', 'vcs0', 'vcs2', 'vecs0'))
rc, out, _ = run(['engines'], d)
check('NO POSH ENGINE' in out and '(vcs1, vecs1)' in out, 'engines: POCS absent verdict')
# RCS silent: void.
d = Engines(driver='i915', live=())
rc, out, _ = run(['engines'], d)
check('result void' in out and 'NO POSH' not in out, 'engines: void without RCS')
# Under i915 without debugfs: refused before mapping.
d = Engines(driver='i915', debugfs=False)
rc, _, err = run(['engines'], d)
check(rc == 2 and 'i915_forcewake_user' in err and not d.mapped, 'engines refused without debugfs')
# Other i915 steps stay refused; 'all' does not include engines.
d = Engines(driver='i915')
rc, _, err = run(['engines', 'forcewake'], d)
check(rc == 2 and 'driver is i915' in err, 'engines + write step refused under i915')
d = Engines()
rc, out, _ = run(['all', '--json'], d)
check('engines' not in json.loads(out)['steps'], "'all' excludes engines")
# Under vfio-pci: register forcewake incl. media domains, every domain released.
d = Engines()
rc, out, _ = run(['engines'], d)
reqs = [C.FW_RENDER[0], C.FW_GT[0]] + [r for r, _ in C.FW_MEDIA.values()]
check(rc == 0 and all(d.writes.count(r) == 2 for r in reqs), 'engines vfio: each domain acquired + released')
check('POCS answers' in out, 'engines vfio: verdict')
check('0x14800=' in out and 'GT config registers' in out, 'engines: GT config dump printed')

if failures:
    sys.exit(1)
print('PASS: coldgt_check (ownership/decode/D3 guards, FLR/GDRST verdicts, forcewake release)')
