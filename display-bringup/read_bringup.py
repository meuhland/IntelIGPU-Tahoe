#!/usr/bin/env python3
"""Print what a loaded ReimsDisplayBringup published (read-only, from ioreg)."""
import plistlib, struct, subprocess, sys

STEPS = {1: 'Preconditions', 2: 'DCOff', 3: 'WaPchClockGate', 4: 'PchHandshake', 5: 'ComboPhy',
         6: 'PowerWell1', 7: 'CdclkPrepare', 8: 'CdclkPll', 9: 'CdclkCtl', 10: 'CdclkVoltage',
         11: 'DbufTracker', 12: 'DbufPower', 13: 'Mbus', 14: 'BwBuddy', 15: 'WaDcpr', 16: 'Done'}
STEPS.update({32 + i: n for i, n in enumerate(
    'Output PowerWell2 PowerWell3 DpllPower DpllConfig DpllEnable DdiClock DdiIoPower TransClock '
    'Infoframes PipeSrc PipeMisc Timings TransMult FrameStart TransConf Linetime PipeChicken MbusDbox '
    'DdiFunc TransEnable PhyKeeper PhyLoadgen PhySusClock PhyTraining PhySwing PhyLanes DdiBuf Verify'.split())})
STEPS.update({64 + i: n for i, n in enumerate(
    'Scanout DbufSlice2 PipeColor Ggtt PlaneConfig PlaneWm PlaneEnable PlaneVerify'.split())})
ACTIONS = ['check', 'skip', 'PLAN', 'WRITE', 'FAIL']
RESULTS = ['OK', 'Planned', 'PreconditionFailed', 'Timeout', 'Unexpected', 'LogFull']

out = subprocess.run(['ioreg', '-r', '-c', 'ReimsDisplayBringup', '-a', '-l'], capture_output=True, check=True).stdout
entries = plistlib.loads(out) if out.strip() else []
if not entries:
    sys.exit('ReimsDisplayBringup is not loaded')
p = entries[0]
for key in ('ReimsBringupPath', 'ReimsBringupExecute', 'ReimsBringupStage', 'PhysicalIdentityVerified',
            'ReimsBringupComplete', 'ReimsBringupBlockedWrites', 'ReimsBringupError'):
    if key in p:
        print(f'{key}: {p[key]}')
if 'ReimsBringupResult' in p:
    r = p['ReimsBringupResult']
    print(f'ReimsBringupResult: {RESULTS[r] if r < len(RESULTS) else r}')
EDID_STATUS = ['OK', 'Busy', 'Nak', 'Timeout', 'BadHeader', 'BadChecksum']
for k in ('ReimsEDIDStatus', 'ReimsEDIDVendor', 'ReimsEDIDProduct',
          'ReimsEDIDPreferredHActive', 'ReimsEDIDPreferredVActive',
          'ReimsEDIDPreferredPixelClockKHz', 'ReimsEDIDExtensions'):
    if k in p:
        v = p[k]
        if k == 'ReimsEDIDStatus' and isinstance(v, int):
            v = EDID_STATUS[v] if v < len(EDID_STATUS) else v
        print(f'{k}: {v}')
for step, action, index, _, reg, before, after in struct.iter_unpack('<BBBBIII', p.get('ReimsBringupLogV1', b'')):
    name = STEPS.get(step, step)
    act = ACTIONS[action] if action < len(ACTIONS) else action
    print(f'  {name:<14} {act:<5} [{index}] {reg:#08x}  {before:#010x} -> {after:#010x}')
