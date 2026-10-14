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

    def __init__(self, driver='vfio-pci', fuses_hot=True, flr_colds=False, gdrst_heals=False,
                 decode=True, d3=False, users=(), device=C.DEVICE, asleep=False):
        self.drv, self.users, self.device = driver, list(users), device
        self.hot, self.flr_colds, self.gdrst_heals = fuses_hot, flr_colds, gdrst_heals
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

    def function_reset(self):
        self.resets += 1
        if self.flr_colds:
            self.hot = False

    def delay_us(self, us):
        pass

    def read32(self, off):
        if self.d3 or not self.cmd & C.CMD_MEMORY:
            return C.ALL_ONES
        if self.asleep and off in (0x9138, 0x913c) and not self.reg.get(C.FW_RENDER[1]):
            return 0
        if off == 0x9138:
            return 1 if self.hot else 0
        if off == 0x913c:
            return 3 if self.hot else 0
        if off == 0x9140:
            return 0xf00ff
        return self.reg.get(off, 0)

    def write32(self, off, v):
        self.writes.append(off)
        if off in (C.FW_RENDER[0], C.FW_GT[0]):
            ack = C.FW_RENDER[1] if off == C.FW_RENDER[0] else C.FW_GT[1]
            self.reg[ack] = v & C.FW_KERNEL
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
check(len(rel) == 4 * 4, "forcewake acquire+release: forcewake 1x, flr 2x (pre+post), gdrst 1x")

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

if failures:
    sys.exit(1)
print('PASS: coldgt_check (ownership/decode/D3 guards, FLR/GDRST verdicts, forcewake release)')
