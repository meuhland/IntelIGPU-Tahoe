#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include "../common/reims_target.hpp"
#include "gt_reset.hpp"

// Phase-6 GT-reset probe (gt_reset.hpp). Default build is read-only: it reads
// the render geometry fuses raw and reports what it would do. The execute
// build (REIMS_GT_RESET_EXECUTE) additionally acquires forcewake, issues
// GEN6_GDRST (writes 0x941c), and re-reads, to test whether resetting the
// cold GT populates the fuses. Run the execute build only from a VM snapshot;
// recover with stop-VM (hookscript rebinds i915) or a host reboot.
#if defined(REIMS_GT_RESET_EXECUTE)
static constexpr bool kExecute=true;
#else
static constexpr bool kExecute=false;
#endif

struct BarIO {
 IOVirtualAddress base;
 uint32_t read(uint32_t o){return *reinterpret_cast<volatile uint32_t*>(base+o);}
 void write(uint32_t o,uint32_t v){*reinterpret_cast<volatile uint32_t*>(base+o)=v;(void)read(o);}
 void delayUS(uint32_t us){IODelay(us);}
};

class ReimsGTResetProbe:public IOService {
 OSDeclareDefaultStructors(ReimsGTResetProbe)
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
  if(!(pci->configRead16(kIOPCIConfigCommand)&kIOPCICommandMemorySpace)){
   entry->release();setProperty("ReimsGTResetError","PCI memory decoding is off; not enabling it");return false;
  }
  auto*map=pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
  entry->release();
  if(!map||map->getLength()<=ReimsGTReset::R::maxOffset){
   OSSafeReleaseNULL(map);setProperty("ReimsGTResetError","BAR0 map failed");return false;
  }
  BarIO io{map->getVirtualAddress()};
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
  setProperty("ReimsGTResetPath",ReimsTarget::kPCIPath);
  setProperty("ReimsGTResetComplete",probe());
  registerService();
  return true;
 }
};
OSDefineMetaClassAndStructors(ReimsGTResetProbe,IOService)
