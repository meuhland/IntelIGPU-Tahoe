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
  pm         PCI PM reset (D3hot -> D0): what a guest PMCSR write or the vfio
             idle D3 does to the device; then raw + forcewake read
  gdrst      forcewake, GEN6_GDRST full reset (i915 sequence), read, release
  all        read, forcewake, flr, pm, gdrst in that order
  engines    which command streamers answer, POCS (0x18000) included (not in 'all')

Everything except `read` writes to the GPU and needs the device on vfio-pci (or
no driver) with no VM holding it. Exception: `engines` under i915 only reads,
holding i915's own debugfs i915_forcewake_user, so it runs in the host's normal
state (VM stopped, i915 bound). Register facts mirror gt-reset-probe/gt_reset.hpp
(verified against i915 v6.6).

Why `engines` (2026-10-16): Apple's TGL kext builds every graphics context with
a POSH context (g_cInitGfxHwCtxParamsForGraphics flags 0x1c, bit 0x10) and points
the POCS at a status page, with no hardware check. If this GT1 has no POCS, the
first context load references an engine that is not there. A live engine reads
MI_MODE/INSTDONE/IMR nonzero; an absent one reads 0 like the fused-off VCS1.
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
FW_KERNEL = 1 << 0  # i915's requester bit; Apple's SafeForceWake uses bit 1 (--fw-bit 1)
MAP_LEN = 0x1e0000  # covers the highest register used (VECS1 0x1d8000 + 0x29c)

# Command streamers (i915 intel_engine_regs.h / Apple's kIGHwCsDesc) and their
# forcewake domain. POCS has no i915 entry: base 0x18000 is from the TGL PRM
# vol 8 and Apple's POCS context template (0x18244...). CCS at 0x1a000 is in
# Apple's table but not exposed by i915 on Gen12 LP. VCS1/VECS1 are fused off
# on ADL-S (i915 platform_engine_mask has VCS0, VCS2, VECS0), the absent baseline.
ENGINES = (('rcs', 0x2000, 'render'), ('pocs', 0x18000, 'render'), ('ccs', 0x1a000, 'gt'),
           ('bcs', 0x22000, 'gt'), ('vcs0', 0x1c0000, 'vdbox0'), ('vcs1', 0x1c4000, 'vdbox1'),
           ('vcs2', 0x1d0000, 'vdbox2'), ('vecs0', 0x1c8000, 'vebox0'), ('vecs1', 0x1d8000, 'vebox1'))
# Read-only per-engine registers: RING_MI_MODE, INSTDONE, IMR, RESET_CTL,
# HWS_PGA, RING_MODE, EXECLIST_STATUS_LO, CONTEXT_CONTROL, ACTHD.
ENGINE_REGS = (('mi_mode', 0x9c), ('instdone', 0x6c), ('imr', 0xa8), ('reset_ctl', 0xd0),
               ('hws_pga', 0x80), ('mode', 0x29c), ('execlist_status', 0x234),
               ('ctx_ctl', 0x244), ('acthd', 0x74))
# Media forcewake (i915 FORCEWAKE_MEDIA_VDBOX/VEBOX_GEN11 and their acks).
FW_MEDIA = {f'vdbox{n}': (0xa540 + 4 * n, 0x0d50 + 4 * n) for n in range(3)}
FW_MEDIA.update({f'vebox{n}': (0xa560 + 4 * n, 0x0d70 + 4 * n) for n in range(2)})
# Every GT register Apple's IntelAccelerator::startGraphicsEngine (0x25100) and
# initModeRegisters (0x28e1a) write, plus the fuse 0x9120 it steers MCR from.
# Read here under i915 they give i915's configuration on this chip, to compare
# offline with the values Apple writes (docs/PORT-RPLS-UHD770.md).
GT_CONFIG = (0x0fdc, 0x2050, 0x2090, 0x20a0, 0x20d8, 0x20e0, 0x20e4, 0x20ec, 0x2214, 0x2248, 0x229c,
             0x2580, 0x25a4, 0x24d0, 0x24d4, 0x24d8, 0x24dc, 0x24e0, 0x24f0, 0x24f4, 0x24f8, 0x24fc,
             0x4a08, 0x4ab8, 0x4ddc, 0x5584, 0x6604, 0x7000, 0x7004, 0x7010, 0x7014, 0x7300, 0x7304,
             0x83a4, 0x9120, 0x9424, 0x9434, 0x94d4, 0x9528, 0xb000, 0xb004, 0xb100, 0xb104, 0xb10c,
             0xb118, 0xb11c, 0xb120, 0xb404, 0xb43c, 0xce90, 0xcf08, 0xe180, 0xe184, 0xe18c, 0xe194,
             0xe48c, 0xe4f0, 0xe4f4, 0xe5f0, 0x14800, 0x18088, 0x180d8, 0x1a050, 0x1a4d0, 0x42080)

PCI_COMMAND, CMD_MEMORY = 0x04, 1 << 1
ALL_ONES = 0xffffffff


class Refused(Exception):
    pass


class SysfsDevice:
    """The real device: sysfs config space, BAR0 mmap, sysfs function reset."""

    def __init__(self, bdf):
        self.bdf = bdf
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

    def user_forcewake_path(self):
        """i915's debugfs i915_forcewake_user for this device, or None.

        While the file is open i915 holds FORCEWAKE_ALL (intel_gt_pm_debugfs.c),
        so registers read live without this script writing to the GPU.
        """
        root = '/sys/kernel/debug/dri'
        try:
            names = sorted(os.listdir(root))
        except OSError:
            return None
        for n in names:
            d = f'{root}/{n}'
            try:
                with open(f'{d}/name') as f:
                    if self.bdf not in f.read() and n != self.bdf:
                        continue
            except OSError:
                if n != self.bdf:
                    continue
            if os.path.exists(f'{d}/i915_forcewake_user'):
                return f'{d}/i915_forcewake_user'
        return None

    def hold_user_forcewake(self):
        return open(self.user_forcewake_path(), 'rb')

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

    def pm_reset(self):
        """Reset through the 'pm' method only (D3hot -> D0), then restore the list."""
        old = self.attr('reset_method') or 'default'
        if 'pm' not in old.split() and old != 'default':
            raise OSError(f'pm reset not supported (reset_method: {old})')
        self.set_attr('reset_method', 'pm')
        try:
            self.set_attr('reset', '1')
        finally:
            self.set_attr('reset_method', old)

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


def classify(f):
    """populated / cold / asleep / unreadable.

    'asleep': every fuse reads 0, media included. Seen on the host right after
    an FLR without forcewake (forcewake then reads the real 1/3/0xe00fa), and
    it is also what VM 113 read: Apple logs ~raw & 0xf00ff for media, so its
    'VEDBOXEnableFuses:0xf00ff' means a raw 0. 'cold' (geometry 0, media live)
    would be a GT that answers without its fuse download; not yet observed.
    """
    if unreadable(f):
        return 'unreadable'
    if populated(f):
        return 'populated'
    return 'cold' if f['media'] else 'asleep'


def acquire_forcewake(dev, req, ack, bit=FW_KERNEL):
    dev.write32(req, (bit << 16) | bit)  # masked write
    for _ in range(100):
        if dev.read32(ack) & bit:
            return True
        dev.delay_us(10)
    return False


def release_forcewake(dev, req, bit=FW_KERNEL):
    dev.write32(req, bit << 16)


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
    bit = getattr(dev, 'fw_bit', FW_KERNEL)
    rec = {'render_acked': acquire_forcewake(dev, *FW_RENDER, bit),
           'gt_acked': acquire_forcewake(dev, *FW_GT, bit)}
    try:
        rec.update(body())
    finally:
        release_forcewake(dev, FW_RENDER[0], bit)
        release_forcewake(dev, FW_GT[0], bit)
    return rec


def step_read(dev):
    return {'raw': read_fuses(dev)}


def step_forcewake(dev):
    rec = {'raw': read_fuses(dev)}
    rec.update(with_forcewake(dev, lambda: {'forcewake': read_fuses(dev)}))
    return rec


def reset_step(dev, reset):
    # Compare forcewake reads on both sides: a raw read can be 0 merely because
    # the GT is asleep (RC6), which would fake a 'revives' verdict.
    before = with_forcewake(dev, lambda: {'f': read_fuses(dev)})['f']
    reset()
    rec = {'before': before, 'raw': read_fuses(dev)}
    rec.update(with_forcewake(dev, lambda: {'forcewake': read_fuses(dev)}))
    return rec


def step_flr(dev):
    return reset_step(dev, dev.function_reset)


def step_pm(dev):
    return reset_step(dev, dev.pm_reset)


def step_gdrst(dev):
    def body():
        fw = read_fuses(dev)
        cleared, loops = gdrst_reset(dev)
        return {'forcewake': fw, 'reset_cleared': cleared, 'reset_loops': loops,
                'after_reset': read_fuses(dev)}
    return with_forcewake(dev, body)


def read_engines(dev):
    return {name: {r: dev.read32(base + off) for r, off in ENGINE_REGS} for name, base, _ in ENGINES}


def read_gt_config(dev):
    return {f'{off:#x}': dev.read32(off) for off in GT_CONFIG}


def answers(regs):
    """A command streamer that exists reads nonzero idle/mask state."""
    return any(regs[k] not in (0, ALL_ONES) for k in ('mi_mode', 'instdone', 'imr', 'reset_ctl'))


def step_engines(dev):
    if getattr(dev, 'via_i915', False):
        f = dev.hold_user_forcewake()
        try:
            return {'held': 'i915_forcewake_user', 'engines': read_engines(dev),
                    'gt_config': read_gt_config(dev)}
        finally:
            f.close()
    bit = getattr(dev, 'fw_bit', FW_KERNEL)
    domains = [FW_RENDER, FW_GT] + [FW_MEDIA[d] for _, _, d in ENGINES if d in FW_MEDIA]
    acked = [acquire_forcewake(dev, req, ack, bit) for req, ack in domains]
    try:
        return {'held': 'register forcewake', 'acked': acked, 'engines': read_engines(dev),
                'gt_config': read_gt_config(dev)}
    finally:
        for req, _ in domains:
            release_forcewake(dev, req, bit)


STEPS = {'read': step_read, 'forcewake': step_forcewake, 'flr': step_flr, 'pm': step_pm,
         'gdrst': step_gdrst, 'engines': step_engines}
ORDER = ('read', 'forcewake', 'flr', 'pm', 'gdrst')  # 'all'
SEQUENCE = ORDER + ('engines',)


def preflight(dev, steps, wake, enable_decode):
    """Identity, ownership and power checks. Returns (info, restore actions)."""
    vendor, device = dev.config(0x00, 2), dev.config(0x02, 2)
    if (vendor, device) != (VENDOR, DEVICE):
        raise Refused(f'device is {vendor:04x}:{device:04x}, expected {VENDOR:04x}:{DEVICE:04x}')
    drv = dev.driver()
    # 'engines' only reads under i915, which holds forcewake for it.
    read_only = {'read', 'engines'} if drv == 'i915' else {'read'}
    writes = any(s not in read_only for s in steps)
    dev.via_i915 = drv == 'i915'
    if 'engines' in steps and drv == 'i915' and not dev.user_forcewake_path():
        raise Refused('engines under i915 needs debugfs i915_forcewake_user '
                      '(run as root; mount -t debugfs none /sys/kernel/debug)')
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
    tag = {'unreadable': 'UNREADABLE (D3 / decode off)', 'populated': 'populated',
           'cold': 'COLD (slice/DSS zero, media live)',
           'asleep': 'ALL ZERO incl. media (GT asleep / forcewake not in effect; the VM signature)'}[classify(f)]
    return (f'slice={f["slice"]:#x} dss={f["dss"]:#x} eu_disable={f["eu_disable"]:#x} '
            f'media={f["media"]:#x}  -> {tag}')


def report(name, rec):
    print(f'[{name}]')
    if 'engines' in rec:
        print(f'  forcewake: {rec["held"]}' + (f' acked={rec["acked"]}' if 'acked' in rec else ''))
        print('  engine  base     ' + ' '.join(f'{r[:8]:>8}' for r, _ in ENGINE_REGS) + '  answers')
        for n, base, _ in ENGINES:
            regs = rec['engines'][n]
            print(f'  {n:6}  {base:#08x} ' + ' '.join(f'{regs[r]:08x}' for r, _ in ENGINE_REGS)
                  + f'  {"yes" if answers(regs) else "no"}')
        cfg = list(rec.get('gt_config', {}).items())
        if cfg:
            print('  GT config registers Apple programs (values as the current owner left them):')
        for i in range(0, len(cfg), 6):
            print('   ' + ' '.join(f'{k:>7}={v:08x}' for k, v in cfg[i:i + 6]))
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
            lines.append('FLR: no change (' + classify(a) + ')')
    pm = results.get('pm')
    if pm:
        b, a = pm['before'], pm['forcewake']
        if populated(b) and not populated(a):
            lines.append(f'PM reset (D3hot->D0) COLDS the GT ({classify(a)}): a D3 in the guest or '
                         'host is the trigger; keep the device in D0 (vfio-pci disable_idle_d3=1, '
                         'and find what puts it in D3 in the guest)')
        elif not populated(b) and populated(a):
            lines.append('PM reset REVIVES the fuses')
        else:
            lines.append('PM reset: no change (' + classify(a) + ')')
    eng = results.get('engines')
    if eng:
        e = eng['engines']
        live = [n for n, _, _ in ENGINES if answers(e[n])]
        lines.append('engines answering: ' + (', '.join(live) or 'none'))
        if not answers(e['rcs']):
            lines.append('RCS does not answer either: forcewake not in effect, engines result void')
        elif answers(e['pocs']):
            lines.append(f'POCS answers like a live engine (MI_MODE {e["pocs"]["mi_mode"]:#x}, INSTDONE '
                         f'{e["pocs"]["instdone"]:#x}): POSH hardware exists, so the POSH contexts '
                         'Apple builds do not reference a missing engine')
        else:
            absent = [n for n in ('vcs1', 'vecs1') if not answers(e[n])]
            lines.append('POCS reads like the absent engines' + (f' ({", ".join(absent)})' if absent else '')
                         + ': NO POSH ENGINE on this GT; Apple builds every graphics context with POSH '
                         '-> prime suspect for the CAT_ERROR (next: candidate with POSH off)')
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
    ap.add_argument('steps', nargs='+', choices=list(SEQUENCE) + ['all'])
    ap.add_argument('--bdf', default=BDF)
    ap.add_argument('--wake', action='store_true',
                    help='set power/control=on for the run (vfio-pci runtime-suspends idle devices to D3)')
    ap.add_argument('--enable-decode', action='store_true',
                    help='turn on PCI memory decode for the run if it is off')
    ap.add_argument('--fw-bit', type=int, choices=range(16), default=0, metavar='N',
                    help="forcewake requester bit (default 0 = i915's; 1 = what Apple's SafeForceWake uses)")
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args(argv)
    wanted = set(args.steps) | (set(ORDER) if 'all' in args.steps else set())
    steps = [s for s in SEQUENCE if s in wanted]

    dev = dev or SysfsDevice(args.bdf)
    dev.fw_bit = 1 << args.fw_bit
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
