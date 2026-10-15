#!/usr/bin/env python3
"""Print what a loaded ReimsGTResetProbe published (read-only, from ioreg).

Shows the render geometry fuses raw, with forcewake, and after GEN6_GDRST, and
whether the reset populated them. i915 gen12_sseu_info_init expects the slice
fuse to read 0x1 and the DSS fuse to be a nonzero mask.
"""
import plistlib
import struct
import subprocess
import sys

out = subprocess.run(['ioreg', '-r', '-c', 'ReimsGTResetProbe', '-a', '-l'],
                     capture_output=True, check=True).stdout
entries = plistlib.loads(out) if out.strip() else []
if not entries:
    sys.exit('ReimsGTResetProbe is not loaded')
p = entries[0]

for key in ('ReimsGTResetPath', 'ReimsGTResetExecuteBuild', 'ReimsGTForcewakeHoldBuild',
            'PhysicalIdentityVerified', 'ForcewakeHeld',
            'ReimsGTResetComplete', 'ReimsGTResetExecuted', 'ReimsGTResetError'):
    if key in p:
        print(f'{key}: {p[key]}')


def fuses(prefix):
    if f'{prefix}Slice' not in p:
        return
    s, d = p[f'{prefix}Slice'], p[f'{prefix}DSS']
    note = ''
    if prefix != 'Raw' or p.get('ReimsGTResetExecuted'):
        note = '  (slice should be 0x1, DSS nonzero on real Gen12)' if (s & 1) == 0 or d == 0 else '  (looks populated)'
    print(f'  {prefix:12} slice={s:#010x} dss={d:#010x} eu_disable={p[f"{prefix}EUDisable"]:#010x} '
          f'media={p[f"{prefix}Media"]:#010x}{note}')


# Interrupt inventory (every build). Types per IOKit: bit 0 = level (else
# edge); 0x10000 = PCI MSI, 0x20000 = MSI-X (xnu IOInterrupts.h values).
def int_type(t):
    kind = 'MSI-X' if t & 0x20000 else 'MSI' if t & 0x10000 else 'INTx'
    return f'{kind} {"level" if t & 1 else "edge"} ({t:#x})'


if 'IntIndexCount' in p:
    n = p['IntIndexCount']
    print('interrupts:')
    types = list(struct.unpack(f'<{n}I', p['IntTypes'])) if n and 'IntTypes' in p else []
    for i, t in enumerate(types):
        print(f'  index {i}: {int_type(t)}')
    print(f'  indices={n} first_error={p.get("IntFirstError", 0):#x} '
          f'specifiers={p.get("IntSpecifierCount")} controllers={p.get("IntControllersPresent")} '
          f'IOPCIMSIMode={p.get("MSIModeProperty")}')
    pin = p.get('IntPin', 0)
    pin_name = 'none' if pin == 0 else ('INT' + 'ABCD'[pin - 1] if pin <= 4 else '?')
    ctl = p.get('MSIControl', 0)
    print(f'  config: interrupt pin={pin} ({pin_name}) line={p.get("IntLine")} '
          f'MSI cap={p.get("MSICapOffset", 0):#x} MSI control={ctl:#06x} (enable={ctl & 1})')
    if n == 0:
        print('  -> no interrupt index on the IGD: a driver here cannot register any interrupt '
              'unless MSI is configured for it first')

print('geometry fuses:')
fuses('Raw')
if p.get('ReimsGTForcewakeHoldBuild'):
    fuses('Held')
    print(f'forcewake acked: render={p.get("RenderForcewakeAcked")} gt={p.get("GTForcewakeAcked")}')
    ok = p.get('HeldFusesPopulated')
    print(f'held fuses populated: {ok}' + (
        '  -> the GT wakes in the guest; leave this loaded and run igpu-start'
        if ok else '  -> the GT does not give real fuses even awake: stop here'))
    sys.exit(0)
if p.get('ReimsGTResetExecuted'):
    fuses('Forcewake')
    fuses('AfterReset')
    print(f'forcewake acked: render={p.get("RenderForcewakeAcked")} gt={p.get("GTForcewakeAcked")}')
    print(f'GDRST cleared: {p.get("ResetCleared")} (loops={p.get("ResetLoops")})')
    verdict = p.get('ResetPopulatedFuses')
    print(f'reset populated the geometry fuses: {verdict}'
          + ('' if verdict else '  -> GDRST is not the lever; try a PCI FLR or the host vfio bind reset'))
else:
    print('(read-only build; rebuild with REIMS_GT_RESET_EXECUTE=1 to test the reset)')
