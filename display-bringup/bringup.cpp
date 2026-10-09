#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSIterator.h>
#include "../common/reims_target.hpp"
#if !defined(REIMS_BRINGUP_STAGE)
#define REIMS_BRINGUP_STAGE 2
#endif
#if REIMS_BRINGUP_STAGE==3
#include "hdmi_output.hpp"
#elif REIMS_BRINGUP_STAGE==2
#include "core_init.hpp"
#else
#error "REIMS_BRINGUP_STAGE must be 2 or 3"
#endif

// Phase 5 display bring-up: stage 2 (display core init) or, with
// REIMS_BRINGUP_STAGE=3, stage 2 then stage 3a (HDMI TC1 at 4K30, no plane;
// hdmi_output.hpp). Builds only with
// REIMS_DISPLAY_BRINGUP=1 and REIMS_TARGET=rpls. Without REIMS_BRINGUP_EXECUTE
// it is a dry run: reads only, and publishes the writes it would make.
// Refuses unless the IGD is isolated (registry device-id FFFF), carries no
// driver other than the boot IONDRVFramebuffer, and decodes memory.
#if defined(REIMS_BRINGUP_EXECUTE)
static constexpr bool kExecute=true;
#else
static constexpr bool kExecute=false;
#endif

// The dry-run build cannot store to the IGD at all: write() only counts.
struct BarIO {
 IOVirtualAddress base;uint32_t blockedWrites=0;
 uint32_t read(uint32_t o){return *reinterpret_cast<volatile uint32_t*>(base+o);}
#if defined(REIMS_BRINGUP_EXECUTE)
 void write(uint32_t o,uint32_t v){*reinterpret_cast<volatile uint32_t*>(base+o)=v;(void)read(o);}
#else
 void write(uint32_t,uint32_t){++blockedWrites;}
#endif
 void delayUS(uint32_t us){IODelay(us);}
 uint64_t nowUS(){uint64_t a=0,ns=0;clock_get_uptime(&a);absolutetime_to_nanoseconds(a,&ns);return ns/1000;}
};

class ReimsDisplayBringup:public IOService {
 OSDeclareDefaultStructors(ReimsDisplayBringup)
 void refuse(const char*why){setProperty("ReimsBringupError",why);}
 // Only the boot framebuffer may be attached to the IGD.
 bool onlyBootFramebuffer(IOPCIDevice*pci){
  auto*it=pci->getChildIterator(gIOServicePlane);if(!it)return false;
  bool ok=true;
  while(auto*child=OSDynamicCast(IORegistryEntry,it->getNextObject()))
   ok&=!strcmp(child->getMetaClass()->getClassName(),"IONDRVFramebuffer");
  it->release();return ok;
 }
 bool bringup(){
  auto*entry=IORegistryEntry::fromPath(ReimsTarget::kPCIPath,nullptr);
  auto*pci=OSDynamicCast(IOPCIDevice,entry);
  if(!pci||pci->isInactive()||!ReimsTarget::isExact(pci)){
   OSSafeReleaseNULL(entry);refuse("target IGD not found at REIMS_PCI_PATH or identity mismatch");return false;
  }
  setProperty("PhysicalIdentityVerified",true);
  const unsigned char isolated[]={0xff,0xff,0,0};
  auto*id=OSDynamicCast(OSData,pci->getProperty("device-id"));
  if(!id||id->getLength()!=sizeof(isolated)||memcmp(id->getBytesNoCopy(),isolated,sizeof(isolated))){
   entry->release();refuse("IGD is not FFFF-isolated");return false;
  }
  if(!onlyBootFramebuffer(pci)){entry->release();refuse("a driver other than IONDRVFramebuffer is attached");return false;}
  if(!(pci->configRead16(kIOPCIConfigCommand)&kIOPCICommandMemorySpace)){
   entry->release();refuse("PCI memory decoding is off; not enabling it");return false;
  }
  IOMemoryMap*map=pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
  if(!map||map->getLength()<0x200000){OSSafeReleaseNULL(map);entry->release();refuse("BAR0 map failed");return false;}
  BarIO io{map->getVirtualAddress()};
#if REIMS_BRINGUP_STAGE==3
  ReimsBringup::HdmiOutput<BarIO> init(io,kExecute);
  const auto result=init.runOutput(ReimsBringup::kUhd30);
#else
  ReimsBringup::CoreInit<BarIO> init(io,kExecute);
  const auto result=init.run();
#endif
  map->release();entry->release();
  setProperty("ReimsBringupResult",uint64_t(result),32);
  setProperty("ReimsBringupBlockedWrites",uint64_t(io.blockedWrites),32);
  if(auto*d=OSData::withBytes(init.log,init.count*sizeof(init.log[0]))){setProperty("ReimsBringupLogV1",d);d->release();}
  return result==(kExecute?ReimsBringup::Result::OK:ReimsBringup::Result::Planned);
 }
public:
 bool start(IOService*provider)override{
  if(!IOService::start(provider))return false;
  setProperty("ReimsBringupExecute",kExecute);
  setProperty("ReimsBringupStage",uint64_t(REIMS_BRINGUP_STAGE),32);
  setProperty("ReimsBringupPath",ReimsTarget::kPCIPath);
  setProperty("ReimsBringupComplete",bringup());
  // Stay attached, even after a refusal, so the result can be read back.
  registerService();
  return true;
 }
};
OSDefineMetaClassAndStructors(ReimsDisplayBringup,IOService)
