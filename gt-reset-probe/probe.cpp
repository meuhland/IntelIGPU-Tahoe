#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSArray.h>
#include "../common/reims_target.hpp"
#include "gt_reset.hpp"

// Phase-6 GT-reset probe (gt_reset.hpp). Default build is read-only: it reads
// the render geometry fuses raw and reports what it would do. The execute
// build (REIMS_GT_RESET_EXECUTE) additionally acquires forcewake, issues
// GEN6_GDRST (writes 0x941c), and re-reads, to test whether resetting the
// cold GT populates the fuses. Run the execute build only from a VM snapshot;
// recover with stop-VM (hookscript rebinds i915) or a host reboot.
//
// The hold build (REIMS_GT_FORCEWAKE_HOLD) does no reset: it acquires
// render+GT forcewake with its own requester bit and keeps it until unload, so
// the GT stays awake through Apple's start (whose standalone forcewake is a
// no-op when getGPUInfo reads the fuses; see gt_reset.hpp). Load it before
// igpu-start and leave it loaded.
//
// Every build also publishes a read-only interrupt inventory of the IGD (Int*,
// MSI* properties): which interrupt indices a driver on it could register,
// without allocating anything. Apple's accelerator, running standalone, falls
// back to a filter interrupt source on its provider; this shows whether any
// interrupt exists there for it.
#if defined(REIMS_GT_RESET_EXECUTE)
static constexpr bool kExecute=true;
#else
static constexpr bool kExecute=false;
#endif
#if defined(REIMS_GT_FORCEWAKE_HOLD)
static constexpr bool kHold=true;
#if defined(REIMS_GT_RESET_EXECUTE)
#error "REIMS_GT_FORCEWAKE_HOLD and REIMS_GT_RESET_EXECUTE are separate builds"
#endif
#else
static constexpr bool kHold=false;
#endif

struct BarIO {
 IOVirtualAddress base;
 uint32_t read(uint32_t o){return *reinterpret_cast<volatile uint32_t*>(base+o);}
 void write(uint32_t o,uint32_t v){*reinterpret_cast<volatile uint32_t*>(base+o)=v;(void)read(o);}
 void delayUS(uint32_t us){IODelay(us);}
};

class ReimsGTResetProbe:public IOService {
 OSDeclareDefaultStructors(ReimsGTResetProbe)
 IOMemoryMap*heldMap=nullptr;  // hold build: BAR0 stays mapped while forcewake is held
 void number(const char*k,uint64_t v,unsigned bits=32){
  if(auto*n=OSNumber::withNumber(v,bits)){setProperty(k,n);n->release();}
 }
 void publishFuses(const char*prefix,const ReimsGTReset::Fuses&f){
  char key[64];
  snprintf(key,sizeof(key),"%sSlice",prefix);number(key,f.slice);
  snprintf(key,sizeof(key),"%sDSS",prefix);number(key,f.dss);
  snprintf(key,sizeof(key),"%sEUDisable",prefix);number(key,f.euDisable);
  snprintf(key,sizeof(key),"%sMedia",prefix);number(key,f.media);
 }
 void publishInterrupts(IOPCIDevice*pci){
  uint32_t types[8]={};unsigned count=0;IOReturn first=kIOReturnSuccess;
  for(int i=0;i<8;++i){
   int t=0;const IOReturn r=pci->getInterruptType(i,&t);
   if(r!=kIOReturnSuccess){first=r;break;}
   types[count++]=uint32_t(t);
  }
  number("IntIndexCount",count);
  number("IntFirstError",uint32_t(first));
  if(count)if(auto*d=OSData::withBytes(types,count*sizeof(types[0]))){setProperty("IntTypes",d);d->release();}
  auto*specs=OSDynamicCast(OSArray,pci->getProperty("IOInterruptSpecifiers"));
  number("IntSpecifierCount",specs?specs->getCount():0);
  setProperty("IntControllersPresent",pci->getProperty("IOInterruptControllers")!=nullptr);
  setProperty("MSIModeProperty",pci->getProperty("IOPCIMSIMode")!=nullptr);
  number("IntPin",pci->configRead8(kIOPCIConfigInterruptPin));
  number("IntLine",pci->configRead8(kIOPCIConfigInterruptLine));
  UInt8 msi=0;
  if(pci->findPCICapability(kIOPCIMSICapability,&msi)&&msi){
   number("MSICapOffset",msi);number("MSIControl",pci->configRead16(UInt8(msi+2)));
  }else number("MSICapOffset",0);
 }
 bool probe(){
  auto*entry=IORegistryEntry::fromPath(ReimsTarget::kPCIPath,nullptr);
  auto*pci=OSDynamicCast(IOPCIDevice,entry);
  if(!pci||pci->isInactive()||!ReimsTarget::isExact(pci)){
   OSSafeReleaseNULL(entry);setProperty("ReimsGTResetError","target IGD not found at REIMS_PCI_PATH or identity mismatch");return false;
  }
  setProperty("PhysicalIdentityVerified",true);
  // FFFF isolation: only run against the cold, driverless, isolated IGD.
  const unsigned char isolated[]={0xff,0xff,0,0};
  auto*id=OSDynamicCast(OSData,pci->getProperty("device-id"));
  if(!id||id->getLength()!=sizeof(isolated)||memcmp(id->getBytesNoCopy(),isolated,sizeof(isolated))){
   entry->release();setProperty("ReimsGTResetError","IGD is not FFFF-isolated");return false;
  }
  publishInterrupts(pci);
  if(!(pci->configRead16(kIOPCIConfigCommand)&kIOPCICommandMemorySpace)){
   entry->release();setProperty("ReimsGTResetError","PCI memory decoding is off; not enabling it");return false;
  }
  auto*map=pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
  entry->release();
  if(!map||map->getLength()<=ReimsGTReset::R::maxOffset){
   OSSafeReleaseNULL(map);setProperty("ReimsGTResetError","BAR0 map failed");return false;
  }
  BarIO io{map->getVirtualAddress()};
  if(kHold){
   const auto h=ReimsGTReset::acquireHold(io);
   heldMap=map;
   publishFuses("Raw",h.raw);
   publishFuses("Held",h.held);
   setProperty("RenderForcewakeAcked",h.renderAcked);
   setProperty("GTForcewakeAcked",h.gtAcked);
   setProperty("ForcewakeHeld",true);
   setProperty("HeldFusesPopulated",ReimsGTReset::populated(h.held));
   return true;
  }
  const auto r=ReimsGTReset::run(io,kExecute);
  map->release();

  setProperty("ReimsGTResetExecuted",r.executed);
  publishFuses("Raw",r.raw);
  if(r.executed){
   publishFuses("Forcewake",r.forcewake);
   publishFuses("AfterReset",r.afterReset);
   setProperty("RenderForcewakeAcked",r.renderAcked);
   setProperty("GTForcewakeAcked",r.gtAcked);
   setProperty("ResetCleared",r.resetCleared);
   number("ResetLoops",r.resetLoops);
   setProperty("ResetPopulatedFuses",ReimsGTReset::resetPopulatedFuses(r));
  }
  return true;
 }
public:
 bool start(IOService*provider)override{
  if(!IOService::start(provider))return false;
  setProperty("ReimsGTResetExecuteBuild",kExecute);
  setProperty("ReimsGTForcewakeHoldBuild",kHold);
  setProperty("ReimsGTResetPath",ReimsTarget::kPCIPath);
  setProperty("ReimsGTResetComplete",probe());
  registerService();
  return true;
 }
 void stop(IOService*provider)override{
  if(heldMap){
   BarIO io{heldMap->getVirtualAddress()};
   ReimsGTReset::releaseHold(io);
   OSSafeReleaseNULL(heldMap);
  }
  IOService::stop(provider);
 }
};
OSDefineMetaClassAndStructors(ReimsGTResetProbe,IOService)
