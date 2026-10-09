#!/usr/bin/env python3
"""Print what a loaded ReimsDisplayProbe published (read-only, from ioreg)."""
import plistlib, struct, subprocess, sys

STATUS = ['OK', 'Route', 'Disabled', 'VariableRefresh', 'PLL', 'Geometry', 'Clock', 'Link']
NAMES = {0x42000: 'FUSE_STATUS', 0x45400: 'PWR_WELL_CTL1 (BIOS)', 0x45404: 'PWR_WELL_CTL2 (driver)',
         0x45454: 'PWR_WELL_CTL_DDI2', 0x45504: 'DC_STATE_EN', 0xc2014: 'SFUSE_STRAP',
         0x64000: 'DDI_BUF_CTL A', 0x64300: 'DDI_BUF_CTL TC1', 0x64400: 'DDI_BUF_CTL TC2',
         0x64500: 'DDI_BUF_CTL TC3', 0x64600: 'DDI_BUF_CTL TC4', 0x51004: 'DSSM (ref clock)',
         0x164280: 'DPCLKA_CFGCR0', 0x1642bc: 'DPCLKA_CFGCR1'}
for i, (en, c0) in enumerate(zip((0x46010, 0x46014, 0x46018, 0x46030),
                                 (0x164284, 0x16428c, 0x164294, 0x1642c0))):
    NAMES.update({en: f'DPLL{i}_ENABLE', c0: f'DPLL{i}_CFGCR0', c0 + 4: f'DPLL{i}_CFGCR1'})
for t, n in enumerate('ABCD'):
    T = t * 0x1000
    NAMES.update({0x60000 + T: f'TRANS_HTOTAL {n}', 0x60008 + T: f'TRANS_HSYNC {n}',
                  0x6000c + T: f'TRANS_VTOTAL {n}', 0x60014 + T: f'TRANS_VSYNC {n}',
                  0x6002c + T: f'TRANS_MULT {n}', 0x60040 + T: f'LINK_M1 {n}', 0x60044 + T: f'LINK_N1 {n}',
                  0x60400 + T: f'TRANS_DDI_FUNC_CTL {n}', 0x60420 + T: f'TRANS_VRR_CTL {n}',
                  0x6042c + T: f'TRANS_VRR_STATUS {n}', 0x70008 + T: f'TRANSCONF {n}',
                  0x70030 + T: f'PIPE_MISC {n}', 0x46140 + t * 4: f'TRANS_CLK_SEL {n}',
                  0x70180 + T: f'PLANE_CTL_1 {n}', 0x7019c + T: f'PLANE_SURF_1 {n}'})

out = subprocess.run(['ioreg', '-r', '-c', 'ReimsDisplayProbe', '-a', '-l'], capture_output=True, check=True).stdout
entries = plistlib.loads(out) if out.strip() else []
if not entries:
    sys.exit('ReimsDisplayProbe is not loaded')
p = entries[0]
for key in ('ReimsDisplayProbePath', 'PhysicalIdentityVerified', 'PCICommand', 'ReimsDisplayProbeComplete',
            'ReimsDisplayProbeAllOnes', 'ReimsDisplayProbeError'):
    if key in p:
        print(f'{key}: {hex(p[key]) if key == "PCICommand" else p[key]}')
for t in p.get('ReimsDisplayProbeTranscoders', []):
    status = STATUS[t['Status']] if t['Status'] < len(STATUS) else t['Status']
    line = f'transcoder {"ABCD"[t["Transcoder"]]}: {status}'
    if status == 'OK':
        line += (f' {t["HActive"]}x{t["VActive"]} @ {t["Refresh1616"] / 65536:.3f} Hz,'
                 f' pixel {t["PixelClockHz"] / 1e6:.3f} MHz, port {t["PortClockHz"] / 1e6:.3f} MHz,'
                 f' DPLL{t["PLL"]}, {t["BPC"]} bpc')
        if 'Port' in t:
            port = 'A' if t['Port'] == 0 else f'TC{t["Port"] - 2}'
            line += f', DDI {port} (PHY {"ABCDE"[t["PHY"]]}), {"DP " + str(t["Lanes"]) + " lanes" if t["DisplayPort"] else "HDMI/DVI"}'
    print(line)
raw = p.get('ReimsDisplayProbeRegistersV1', b'')
for offset, value in struct.iter_unpack('<II', raw):
    print(f'  {offset:#08x} {value:#010x}  {NAMES.get(offset, "")}')
