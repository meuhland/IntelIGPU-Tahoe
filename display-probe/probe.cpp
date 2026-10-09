#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include "../common/reims_target.hpp"
#include "../desktop-reset-recovery-20260917/source/desktop-link/display_timing.hpp"

// Read-only display state probe. Maps BAR0 of the target IGD and only loads
// from it: never writes MMIO or PCI config space, and refuses rather than
// enable memory decoding. Results stay on this service until kext unload.
// Extra diagnostics (Linux i915 v7.2): SKL_FUSE_STATUS, HSW_PWR_WELL_CTL1/2,
// ICL_PWR_WELL_CTL_DDI2, DC_STATE_EN, SFUSE_STRAP, DDI_BUF_CTL A/TC1-TC4,
// PLANE_CTL_1/PLANE_SURF_1 for pipes A-D.
class ReimsDisplayProbe:public IOService {
 OSDeclareDefaultStructors(ReimsDisplayProbe)
 static constexpr uint32_t extra[]={0x42000,0x45400,0x45404,0x45454,0x45504,0xc2014,
  0x64000,0x64300,0x64400,0x64500,0x64600,
  0x70180,0x7019c,0x71180,0x7119c,0x72180,0x7219c,0x73180,0x7319c};
 static constexpr unsigned extraCount=sizeof(extra)/sizeof(extra[0]);
 static constexpr uint32_t barMinimum=0x200000; // display MMIO used here
 void number(OSDictionary*d,const char*k,uint64_t v,unsigned bits=32){
  if(auto*n=OSNumber::withNumber(v,bits)){d->setObject(k,n);n->release();}
 }
 void publishMode(OSArray*out,unsigned t,ReimsDisplayTiming::Status status,const ReimsDisplayTiming::Mode&m){
  auto*d=OSDictionary::withCapacity(16);if(!d)return;
  number(d,"Transcoder",t);number(d,"Status",uint64_t(status));
  if(status==ReimsDisplayTiming::Status::OK){
   number(d,"HActive",m.hActive);number(d,"VActive",m.vActive);
   number(d,"HTotal",m.hTotal);number(d,"VTotal",m.vTotal);
   number(d,"HSyncStart",m.hSyncStart);number(d,"HSyncEnd",m.hSyncEnd);
   number(d,"VSyncStart",m.vSyncStart);number(d,"VSyncEnd",m.vSyncEnd);
   number(d,"PixelClockHz",m.pixelClockHz,64);number(d,"PortClockHz",m.portClockHz,64);
   number(d,"Refresh1616",m.refresh1616);number(d,"PLL",m.pll);number(d,"BPC",m.bpc);
   number(d,"Lanes",m.lanes);
#if defined(REIMS_TARGET_RPLS)
   number(d,"Port",m.port);number(d,"PHY",m.phy);
   d->setObject("DisplayPort",m.dp?kOSBooleanTrue:kOSBooleanFalse);
#endif
  }
  out->setObject(d);d->release();
 }
 bool probe(){
  auto*entry=IORegistryEntry::fromPath(ReimsTarget::kPCIPath,nullptr);
  auto*pci=OSDynamicCast(IOPCIDevice,entry);
  if(!pci||pci->isInactive()||!ReimsTarget::isExact(pci)){
   OSSafeReleaseNULL(entry);setProperty("ReimsDisplayProbeError","target IGD not found at REIMS_PCI_PATH or identity mismatch");return false;
  }
  setProperty("PhysicalIdentityVerified",true);
  const uint16_t command=pci->configRead16(kIOPCIConfigCommand);
  setProperty("PCICommand",uint64_t(command),16);
  if(!(command&kIOPCICommandMemorySpace)){
   entry->release();setProperty("ReimsDisplayProbeError","PCI memory decoding is off; not enabling it");return false;
  }
  IOMemoryMap*map=pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
  if(!map||map->getLength()<barMinimum){
   OSSafeReleaseNULL(map);entry->release();setProperty("ReimsDisplayProbeError","BAR0 map failed");return false;
  }
  const auto base=map->getVirtualAddress();
  auto read=[base](uint32_t o){return *reinterpret_cast<volatile uint32_t*>(base+o);};
  ReimsDisplayTiming::Snapshot s;
  uint32_t pairs[(ReimsDisplayTiming::count+extraCount)*2];
  for(unsigned i=0;i<ReimsDisplayTiming::count;++i){
   s.value[i]=read(ReimsDisplayTiming::offsets[i]);
   pairs[2*i]=ReimsDisplayTiming::offsets[i];pairs[2*i+1]=s.value[i];
  }
  for(unsigned i=0;i<extraCount;++i){
   const unsigned j=ReimsDisplayTiming::count+i;pairs[2*j]=extra[i];pairs[2*j+1]=read(extra[i]);
  }
  map->release();entry->release();
  // A device that stopped responding reads all-ones everywhere.
  bool allOnes=true;
  for(unsigned i=0;i<sizeof(pairs)/sizeof(pairs[0])/2;++i)allOnes&=pairs[2*i+1]==0xffffffffU;
  setProperty("ReimsDisplayProbeAllOnes",allOnes);
  if(auto*d=OSData::withBytes(pairs,sizeof(pairs))){setProperty("ReimsDisplayProbeRegistersV1",d);d->release();}
  auto*modes=OSArray::withCapacity(4);if(!modes)return false;
  ReimsDisplayTiming::Mode m;
#if defined(REIMS_TARGET_RPLS)
  for(unsigned t=0;t<ReimsDisplayTiming::transcoders;++t)publishMode(modes,t,ReimsDisplayTiming::decode(s,t,m),m);
#else
  publishMode(modes,0,ReimsDisplayTiming::decode(s,m),m);
#endif
  setProperty("ReimsDisplayProbeTranscoders",modes);modes->release();
  return true;
 }
public:
 bool start(IOService*provider)override{
  if(!IOService::start(provider))return false;
  setProperty("ReimsDisplayProbeReadOnly",true);
  setProperty("ReimsDisplayProbePath",ReimsTarget::kPCIPath);
  setProperty("ReimsDisplayProbeComplete",probe());
  // Stay attached, even after a refusal, so the result can be read back.
  registerService();
  return true;
 }
};
OSDefineMetaClassAndStructors(ReimsDisplayProbe,IOService)
