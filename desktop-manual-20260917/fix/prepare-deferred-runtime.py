"""Prepare only: defer TGL service publication until explicit display commit."""
from pathlib import Path
import hashlib, json, os, plistlib, shutil, struct, subprocess, argparse
base=Path(__file__).resolve().parent
# Same selector as the kernel builds (scripts/target-flags.sh). adlp output is
# byte-identical to upstream; rpls adds the GT1 L3 bank patch below.
target_name=os.environ.get('REIMS_TARGET','adlp')
assert target_name in ('adlp','rpls'),f'Unknown REIMS_TARGET={target_name}'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('source',type=Path,help='Locally obtained compatible TGL runtime bundle (not distributed)')
source=parser.parse_args().source.resolve()
target=base/'ReimsTGLManualRuntime.kext'
b=(source/'Contents/MacOS/AppleIntelTGLGraphics').read_bytes()
# Accepted inputs. The sle_Internal copy shares the LC_UUID of the community
# le copy and differs from it only in code-signature bytes; it passes every
# offset and relocation check below. Byte equality of its code with the
# upstream input is unverified.
accepted={
    'ae99582bd5a945494ee684d339ac1abd0526828bcd3ea981239c0fd38f794d47':'upstream verified input',
    '890735a93a19d3ddda24ff31bda3075dbd145fb7b26c11769f33c4564a8027ac':'sle_Internal AppleIntelTGLGraphics 16.0.0',
}
source_sha256=hashlib.sha256(b).hexdigest()
assert source_sha256 in accepted,source_sha256
# Validate vtable relocation, not merely the call's apparent offset.
cursor=32
for _ in range(struct.unpack_from('<I',b,16)[0]):
    command,size=struct.unpack_from('<II',b,cursor)
    if command==2:sym=struct.unpack_from('<IIII',b,cursor+8)
    if command==0x1b:uuid_offset=cursor+8
    if command==11:dyn=struct.unpack_from('<18I',b,cursor+8)
    cursor+=size
names=[]; values={}
for i in range(sym[1]):
    offset,kind,section,desc,value=struct.unpack_from('<IBBHQ',b,sym[0]+16*i)
    name=b[sym[2]+offset:b.index(b'\0',sym[2]+offset)].decode()
    names.append(name);values[name]=value
relocs={}
for i in range(dyn[15]):
    address,info=struct.unpack_from('<II',b,dyn[14]+8*i)
    if info>>27&1:relocs[address]=names[info&0xffffff]
assert values['__ZN16IntelAccelerator5startEP9IOService']==0x23ecc
assert relocs[values['__ZTV16IntelAccelerator']+16+0x5b0]=='__ZN9IOService15registerServiceEj'
site=0x246b5
assert b[site-12:site+6]==bytes.fromhex('498b45004531ff4c89ef31f6ff90b0050000')
assert not any(site<=a<site+6 for a in relocs)
edits=[(site,b'\x90'*6)]
# RPL-S SKU check. IntelAccelerator::probe compares vendor|device<<16 with
# 0x9A408086 and 0x9A488086 (both store GT tier 2 at +0x1120) and calls the
# "Failed to map Device ID to GPU SKU" panic for anything else; 0xA7808086 is
# not listed. Make the last compare unconditional (je -> jmp) so an unlisted ID
# takes the tier-2 store. 0xA7808086 is below 0xDEAFBEEE as a signed int, so
# the sentinel branch before it is not taken. Physical identity stays FFFF at
# boot; the gate still only adds a personality for a780 rev 04.
sku_site=0x23c35
if target_name=='rpls':
    assert values['__ZN16IntelAccelerator5probeEP9IOServicePi']==0x238e2
    assert b[sku_site-28:sku_site+7]==bytes.fromhex('488b75b88b3e'+'81ffeebeafde7f15'+'81ff8680409a742d'+'81ff8680489a'+'7425'+'e8cecb0600')
    assert not any(sku_site<=a<sku_site+2 for a in relocs)
    edits.append((sku_site,b'\xeb\x25'))
# RPL-S GT1 (1 slice, 4 subslices from the fuses). getGPUInfo's TGL-LP GT2 path
# sets L3BankCount (+0x1164) to 4 for that shape only when bit 0x20 of the
# flags at +0x1190 is set, else logs "Invalid NumSlice or NumSubSlices" and
# leaves it unset. That bit is the simulator flag (setInSimulator sets it for
# the 0xDEAFBEEF/0xFF208086 sentinel IDs) and changes the GuC paths, so do not
# set it: drop its test instead (je +0x12 -> 2 NOPs). ADL-P's 1x12 takes the
# branch before this one.
l3_site=0x287ea
if target_name=='rpls':
    assert values['__ZN16IntelAccelerator10getGPUInfoEv']==0x2847e
    assert b[l3_site-11:l3_site+18]==bytes.fromhex('83fe01751a83e0204885c0'+'7412'+'83fa04750d41c7876411000004000000')
    assert not any(l3_site<=a<l3_site+2 for a in relocs)
    edits.append((l3_site,b'\x90'*2))
# RPL-S fuse injection. On the cold, GOP-less passed-through GT the render
# geometry fuse mirrors read 0 (hardware 2026-10-14: SliceCountFuse and
# DualSubSliceCountFuse both 0), so getGPUInfo builds a 0-EU GPU, hits the
# "Invalid NumSlice or NumSubSlices" path, and never reaches the L3 branch
# above (which is why the L3 patch alone left L3BankCount=0). i915
# gen12_sseu_info_init expects GEN11_GT_SLICE_ENABLE (0x9138) = 0x1 and derives
# subslices from GEN12_GT_GEOMETRY_DSS_ENABLE (0x913c). Force the two fuse loads
# to immediates for a UHD 770 GT1: slice fuse -> 1 (popcount -> NumSlices 1),
# DSS fuse -> 3 (popcount 2, doubled -> NumSubSlices 4). EU-disable (0x9134)
# already reads 0, so MaxEUPerSubSlice stays 8 and ExecutionUnitCount becomes
# 32. That makes the SKU-tier-2 path take 1x4, clearing the error and (with the
# L3 patch) setting L3BankCount=4. This is injection, not GT cold-init: if the
# GT genuinely cannot execute, the hang moves to the start-up stamp wait.
slice_fuse_site=0x284cc  # movl 0x9138(%rax), %r12d
dss_fuse_site=0x284d3    # movl 0x913c(%rax), %ecx
if target_name=='rpls':
    assert b[slice_fuse_site:slice_fuse_site+7]==bytes.fromhex('448ba038910000')
    assert b[dss_fuse_site:dss_fuse_site+6]==bytes.fromhex('8b883c910000')
    assert not any(slice_fuse_site<=a<slice_fuse_site+7 for a in relocs)
    assert not any(dss_fuse_site<=a<dss_fuse_site+6 for a in relocs)
    edits.append((slice_fuse_site,bytes.fromhex('41bc01000000')+b'\x90'))  # mov r12d,1 ; nop
    edits.append((dss_fuse_site,bytes.fromhex('b903000000')+b'\x90'))      # mov ecx,3 ; nop
patched=bytearray(b)
for at,new in edits:patched[at:at+len(new)]=new
# Every byte outside the edits is unchanged.
restored=bytearray(patched)
for at,new in edits:
    assert patched[at:at+len(new)]==new
    restored[at:at+len(new)]=b[at:at+len(new)]
assert restored==b
deferred_uuid=hashlib.sha256(patched).digest()[:16]
patched[uuid_offset:uuid_offset+16]=deferred_uuid
(base/'manual-gate/deferred_uuid.hpp').write_text('static const unsigned char deferredUUID[]={'+','.join(hex(x) for x in deferred_uuid)+'};\n')
if target.exists(): raise SystemExit('Candidate already exists; do not overwrite a reviewed release.')
shutil.copytree(source,target)
(target/'Contents/MacOS/AppleIntelTGLGraphics').write_bytes(patched)
p=target/'Contents/Info.plist';info=plistlib.loads(p.read_bytes())
# No GPU personalities in the installed candidate. Only the explicit controller
# may add a current-session personality. Physical identity remains FFFF at boot.
info['IOKitPersonalities']={}
info['CFBundleVersion']=info['CFBundleShortVersionString']='16.0.3'
# The gate and DesktopLink link against this kext (OSBundleLibraries:
# AppleIntelTGLGraphics). A provider only satisfies a library dependency when
# it declares a compatible version, and the stock Apple binary omits it. The
# two dependents disagree on the version they require (gate 16.0.3,
# DesktopLink 16.0.0), so advertise a range that covers both: current version
# 16.0.3 (ceiling) and compatible back to 16.0.0 (floor).
info['OSBundleCompatibleVersion']='16.0.0'
info['ReimsManualPublicationDeferred']=True
p.write_bytes(plistlib.dumps(info))
subprocess.run(['codesign','--force','--sign','-',str(target)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(target)],check=True)
record={'source_sha256':source_sha256,'candidate_sha256':hashlib.sha256((target/'Contents/MacOS/AppleIntelTGLGraphics').read_bytes()).hexdigest(),'deferred_call_file_offset':hex(site),'deferred_call':'IOService::registerService(unsigned int)','functional_text_change_bytes':sum(len(new) for _,new in edits),'candidate_uuid':deferred_uuid.hex(),'automatic_personalities':0,'installed':False,'hardware_tested':False}
if target_name=='rpls':
    record['target']='rpls'
    record['sku_check_file_offset']=hex(sku_site)
    record['sku_check_patch']='IntelAccelerator::probe: unlisted device IDs take the TGL-LP GT2 SKU instead of the panic'
    record['gt1_l3_bank_file_offset']=hex(l3_site)
    record['gt1_l3_bank_patch']='IntelAccelerator::getGPUInfo: 1x4 GT2 path sets L3BankCount=4 without flag 0x20'
    record['fuse_injection_file_offsets']=[hex(slice_fuse_site),hex(dss_fuse_site)]
    record['fuse_injection_patch']='IntelAccelerator::getGPUInfo: slice fuse 0x9138->1, DSS fuse 0x913c->3 (NumSlices 1, NumSubSlices 4, EU 32)'
(base/'deferred-runtime.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
