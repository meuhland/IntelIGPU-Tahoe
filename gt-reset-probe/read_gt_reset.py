#!/usr/bin/env python3
"""Print what a loaded ReimsGTResetProbe published (read-only, from ioreg).

Shows the render geometry fuses raw, with forcewake, and after GEN6_GDRST, and
whether the reset populated them. i915 gen12_sseu_info_init expects the slice
fuse to read 0x1 and the DSS fuse to be a nonzero mask.
"""
import plistlib
import subprocess
import sys

out = subprocess.run(['ioreg', '-r', '-c', 'ReimsGTResetProbe', '-a', '-l'],
                     capture_output=True, check=True).stdout
entries = plistlib.loads(out) if out.strip() else []
if not entries:
    sys.exit('ReimsGTResetProbe is not loaded')
p = entries[0]

for key in ('ReimsGTResetPath', 'ReimsGTResetExecuteBuild', 'PhysicalIdentityVerified',
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


print('geometry fuses:')
fuses('Raw')
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
