#!/usr/bin/env python3
"""Host-side cold-GT experiment for the RPL-S iGPU (no macOS VM involved).

The phase-6 hang comes from the render geometry fuse mirrors (0x9138/0x913c)
reading 0 inside VM 113, while the host under i915 reads 1/3. Something in the
i915 -> vfio-pci -> QEMU handoff leaves the GT cold. This script reproduces that
handoff on the host, one step at a time, and reads the fuses after each step,
so the cause and the cheap fixes (GDRST, FLR) are answered in a few minutes of
transcoding downtime instead of a VM window.

Run as root on the Proxmox host with VM 113 stopped. Steps:

  read       raw fuse read, no writes (safe under any driver)
  forcewake  raw read, render+GT forcewake, read, release
  flr        sysfs function reset (what vfio/QEMU applies), then raw + forcewake read
  gdrst      forcewake, GEN6_GDRST full reset (i915 sequence), read, release
  all        read, forcewake, flr, gdrst in that order

Everything except `read` writes to the GPU and needs the device on vfio-pci (or
no driver) with no VM holding it. Register facts mirror gt-reset-probe/gt_reset.hpp
(verified against i915 v6.6).
"""
import argparse
import json
import mmap
import os
import sys
import time

BDF = '0000:00:02.0'
VENDOR, DEVICE = 0x8086, 0xa780

FUSES = (('slice', 0x9138), ('dss', 0x913c), ('eu_disable', 0x9134), ('media', 0x9140))
GDRST, GRDOM_FULL = 0x941c, 1 << 0
FW_RENDER = (0xa278, 0xd84)
FW_GT = (0xa188, 0x130044)
FW_KERNEL = 1 << 0
MAP_LEN = 0x140000  # covers the highest register used (0x130044)

PCI_COMMAND, CMD_MEMORY = 0x04, 1 << 1
ALL_ONES = 0xffffffff


class Refused(Exception):
    pass


class SysfsDevice:
    """The real device: sysfs config space, BAR0 mmap, sysfs function reset."""

    def __init__(self, bdf):
        self.path = f'/sys/bus/pci/devices/{bdf}'
        self._mm = None
        self._regs = None

    def attr(self, name):
        try:
            with open(f'{self.path}/{name}') as f:
                return f.read().strip()
        except OSError:
            return None

    def set_attr(self, name, value):
        with open(f'{self.path}/{name}', 'w') as f:
            f.write(value)

    def driver(self):
        link = f'{self.path}/driver'
        return os.path.basename(os.readlink(link)) if os.path.islink(link) else None

    def config(self, offset, size):
        with open(f'{self.path}/config', 'rb') as f:
            f.seek(offset)
            return int.from_bytes(f.read(size), 'little')

    def set_config16(self, offset, value):
        with open(f'{self.path}/config', 'r+b') as f:
            f.seek(offset)
            f.write(value.to_bytes(2, 'little'))

    def vfio_users(self):
        """PIDs holding this device's vfio group or cdev open (a running VM)."""
        targets = set()
        group = f'{self.path}/iommu_group'
        if os.path.islink(group):
            targets.add(f'/dev/vfio/{os.path.basename(os.readlink(group))}')
        try:
            targets.update(f'/dev/vfio/devices/{n}' for n in os.listdir(f'{self.path}/vfio-dev'))
        except OSError:
            pass
        users = set()
        for pid in filter(str.isdigit, os.listdir('/proc')):
            try:
                for fd in os.listdir(f'/proc/{pid}/fd'):
                    if os.readlink(f'/proc/{pid}/fd/{fd}') in targets:
                        users.add(int(pid))
            except OSError:
                continue
        return sorted(users)

    def map_bar0(self):
        fd = os.open(f'{self.path}/resource0', os.O_RDWR | os.O_SYNC)
        try:
            self._mm = mmap.mmap(fd, MAP_LEN, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        finally:
            os.close(fd)
        # A 'I' memoryview turns each index into one aligned 32-bit load/store,
        # which MMIO needs (plain slicing may copy bytewise).
        self._regs = memoryview(self._mm).cast('I')

    def read32(self, off):
        return self._regs[off // 4]

    def write32(self, off, value):
        self._regs[off // 4] = value & ALL_ONES

    def delay_us(self, us):
        time.sleep(us / 1e6)

    def function_reset(self):
        self.set_attr('reset', '1')

    def close(self):
        if self._regs is not None:
            self._regs.release()
            self._regs = None
        if self._mm is not None:
            self._mm.close()
            self._mm = None


def pm_state(dev):
    """D-state from the PCI PM capability's PMCSR (bits 1:0), or None."""
    if not dev.config(0x06, 2) & (1 << 4):  # status: capability list
        return None
    ptr, seen = dev.config(0x34, 1) & 0xfc, 0
    while ptr and seen < 48:
        if dev.config(ptr, 1) == 0x01:
            return f'D{dev.config(ptr + 4, 2) & 0x3}'
        ptr, seen = dev.config(ptr + 1, 1) & 0xfc, seen + 1
    return None


def read_fuses(dev):
    return {name: dev.read32(off) for name, off in FUSES}


def populated(f):
    """i915 gen12_sseu_info_init expects slice 0x1 and a nonzero DSS mask."""
    return bool(f['slice'] & 1) and f['dss'] != 0


def unreadable(f):
    return any(v == ALL_ONES for v in f.values())


def acquire_forcewake(dev, req, ack):
    dev.write32(req, (FW_KERNEL << 16) | FW_KERNEL)  # masked write
    for _ in range(100):
        if dev.read32(ack) & FW_KERNEL:
            return True
        dev.delay_us(10)
    return False


def release_forcewake(dev, req):
    dev.write32(req, FW_KERNEL << 16)


def gdrst_reset(dev, mask=GRDOM_FULL):
    """gen6_hw_domain_reset: write, poll self-clear (2 ms), twice pre-MTL."""
    loops = 0
    for _ in range(2):
        dev.write32(GDRST, mask)
        cleared = False
        for _ in range(200):
            if dev.read32(GDRST) & mask == 0:
                cleared = True
                break
            dev.delay_us(10)
        loops += 1
        if not cleared:
            return False, loops
    return True, loops


def with_forcewake(dev, body):
    rec = {'render_acked': acquire_forcewake(dev, *FW_RENDER),
           'gt_acked': acquire_forcewake(dev, *FW_GT)}
    try:
        rec.update(body())
    finally:
        release_forcewake(dev, FW_RENDER[0])
        release_forcewake(dev, FW_GT[0])
    return rec


def step_read(dev):
    return {'raw': read_fuses(dev)}


def step_forcewake(dev):
    rec = {'raw': read_fuses(dev)}
    rec.update(with_forcewake(dev, lambda: {'forcewake': read_fuses(dev)}))
    return rec


def step_flr(dev):
    # Compare forcewake reads on both sides: a raw read can be 0 merely because
    # the GT is asleep (RC6), which would fake a 'revives' verdict.
    before = with_forcewake(dev, lambda: {'f': read_fuses(dev)})['f']
    dev.function_reset()
    rec = {'before': before, 'raw': read_fuses(dev)}
    rec.update(with_forcewake(dev, lambda: {'forcewake': read_fuses(dev)}))
    return rec


def step_gdrst(dev):
    def body():
        fw = read_fuses(dev)
        cleared, loops = gdrst_reset(dev)
        return {'forcewake': fw, 'reset_cleared': cleared, 'reset_loops': loops,
                'after_reset': read_fuses(dev)}
    return with_forcewake(dev, body)


STEPS = {'read': step_read, 'forcewake': step_forcewake, 'flr': step_flr, 'gdrst': step_gdrst}
ORDER = ('read', 'forcewake', 'flr', 'gdrst')


def preflight(dev, steps, wake, enable_decode):
    """Identity, ownership and power checks. Returns (info, restore actions)."""
    vendor, device = dev.config(0x00, 2), dev.config(0x02, 2)
    if (vendor, device) != (VENDOR, DEVICE):
        raise Refused(f'device is {vendor:04x}:{device:04x}, expected {VENDOR:04x}:{DEVICE:04x}')
    drv = dev.driver()
    writes = any(s != 'read' for s in steps)
    if writes and drv not in ('vfio-pci', None):
        raise Refused(f'driver is {drv}; GPU writes need vfio-pci or no driver '
                      f'(run igpu-hookscript.sh 113 pre-start first)')
    if writes and drv == 'vfio-pci':
        users = dev.vfio_users()
        if users:
            raise Refused(f'vfio device is open by PID(s) {users}; stop the VM first')
    restore = []
    # Before --wake: 'suspended' means vfio-pci put the idle device in D3, so
    # any cold state seen after waking may come from that D3 round trip.
    status_before = dev.attr('power/runtime_status')
    control = dev.attr('power/control')
    if wake and control not in (None, 'on'):
        dev.set_attr('power/control', 'on')
        restore.append(('power/control', control))
    cmd = dev.config(PCI_COMMAND, 2)
    if not cmd & CMD_MEMORY:
        if not enable_decode:
            raise Refused('memory decode is off (PCI command bit 1); rerun with --enable-decode')
        dev.set_config16(PCI_COMMAND, cmd | CMD_MEMORY)
        restore.append(('command', cmd))
    info = {'driver': drv, 'runtime_status_before': status_before,
            'power_control': dev.attr('power/control'),
            'runtime_status': dev.attr('power/runtime_status'),
            'pm_state': pm_state(dev), 'reset_method': dev.attr('reset_method'),
            'command': dev.config(PCI_COMMAND, 2)}
    return info, restore


def undo(dev, restore):
    for what, value in reversed(restore):
        try:
            if what == 'command':
                dev.set_config16(PCI_COMMAND, value)
            else:
                dev.set_attr(what, value)
        except OSError as e:
            print(f'warning: could not restore {what}: {e}', file=sys.stderr)


def fmt(f):
    if f is None:
        return '-'
    tag = 'UNREADABLE (D3 / decode off)' if unreadable(f) else (
        'populated' if populated(f) else 'COLD (slice/DSS zero)')
    return (f'slice={f["slice"]:#x} dss={f["dss"]:#x} eu_disable={f["eu_disable"]:#x} '
            f'media={f["media"]:#x}  -> {tag}')


def report(name, rec):
    print(f'[{name}]')
    for key in ('before', 'raw', 'forcewake', 'after_reset'):  # before = forcewake read pre-FLR
        if key in rec:
            print(f'  {key:12} {fmt(rec[key])}')
    if 'render_acked' in rec:
        print(f'  forcewake acked: render={rec["render_acked"]} gt={rec["gt_acked"]}')
    if 'reset_cleared' in rec:
        print(f'  GDRST cleared: {rec["reset_cleared"]} (loops={rec["reset_loops"]})')


def verdict(results, info=None):
    """One line per question the experiment answers."""
    lines = []
    was_d3 = (info or {}).get('runtime_status_before') == 'suspended'
    raw = results.get('read', {}).get('raw')
    fw = results.get('forcewake', {}).get('forcewake')
    # Forcewake is the meaningful comparison: Apple forcewakes in the guest and
    # still read 0. A raw 0 may only mean the GT is asleep.
    if fw is not None and not unreadable(fw):
        lines.append('current state (forcewake read): fuses ' + ('populated' if populated(fw) else 'COLD'))
        if not populated(fw):
            lines.append('cold before any FLR: ' + (
                'the i915 unbind or the vfio-pci idle D3 round trip; rerun with vfio-pci '
                'disable_idle_d3=1 to tell them apart'
                if was_d3 else 'i915 unbind / vfio-pci bind itself left it cold (no D3 involved)'))
        if raw is not None and not unreadable(raw) and populated(fw) and not populated(raw):
            lines.append('raw read was 0 only because the GT was asleep (forcewake fixes the read)')
    elif raw is not None and not unreadable(raw):
        lines.append('current state (raw read, no forcewake): fuses '
                     + ('populated' if populated(raw) else 'zero; may only mean GT asleep, run forcewake'))
    flr = results.get('flr')
    if flr:
        b, a = flr['before'], flr['forcewake']
        if populated(b) and not populated(a):
            lines.append('FLR COLDS the GT (QEMU/vfio resets on open): try disabling the reset, '
                         'echo > reset_method, before the VM starts')
        elif not populated(b) and populated(a):
            lines.append('FLR REVIVES the fuses (option 3 works)')
        else:
            lines.append('FLR: no change (' + ('populated' if populated(a) else 'cold') + ')')
    gd = results.get('gdrst')
    if gd:
        b, a = gd['forcewake'], gd['after_reset']
        if not populated(b) and populated(a):
            lines.append('GDRST REVIVES the fuses (option 2 works; run the in-guest probe)')
        elif not populated(a):
            lines.append('GDRST does not re-populate (option 2 is not the lever)')
        else:
            lines.append('GDRST: fuses populated before and after')
    return lines


def main(argv=None, dev=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('steps', nargs='+', choices=list(STEPS) + ['all'])
    ap.add_argument('--bdf', default=BDF)
    ap.add_argument('--wake', action='store_true',
                    help='set power/control=on for the run (vfio-pci runtime-suspends idle devices to D3)')
    ap.add_argument('--enable-decode', action='store_true',
                    help='turn on PCI memory decode for the run if it is off')
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args(argv)
    steps = list(ORDER) if 'all' in args.steps else [s for s in ORDER if s in args.steps]

    dev = dev or SysfsDevice(args.bdf)
    try:
        info, restore = preflight(dev, steps, args.wake, args.enable_decode)
    except Refused as e:
        print(f'refused: {e}', file=sys.stderr)
        return 2
    results = {}
    try:
        dev.map_bar0()
        for s in steps:
            results[s] = STEPS[s](dev)
            if unreadable(results[s].get('raw') or results[s].get('forcewake') or {}):
                results[s]['stopped'] = 'registers read all-ones; device is in D3 or decode is off (try --wake)'
                break
    finally:
        dev.close()
        undo(dev, restore)

    if args.json:
        print(json.dumps({'device': info, 'steps': results, 'verdict': verdict(results, info)}, indent=1))
        return 0
    print('device: ' + '  '.join(f'{k}={v:#06x}' if k == 'command' else f'{k}={v}' for k, v in info.items()))
    for s, rec in results.items():
        report(s, rec)
        if 'stopped' in rec:
            print(f'  stopped: {rec["stopped"]}')
    for line in verdict(results, info):
        print(f'=> {line}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
