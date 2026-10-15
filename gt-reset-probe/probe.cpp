#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSArray.h>
#include "../common/reims_target.hpp"
#include "gt_reset.hpp"
#if defined(REIMS_GT_FORCEWAKE_HOLD)
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/thread_act.h>
#include "gt_watch.hpp"
#endif

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
// igpu-start and leave it loaded. While held it also runs the GT watch
// (gt_watch.hpp): a sampler thread reads the ReimsGTSampler registers every
// 250 ms and a reporter thread, which never touches MMIO, prints them over
// serial (IOLog) and reports a sampler stuck in a register read. Kernel
// threads, not an IOKit work loop, so a wedged accelerator start cannot hold
// them up. Decode the capture with display-bringup/read_gt_serial.py.
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

#if defined(REIMS_GT_FORCEWAKE_HOLD)
struct GTWatch {
 IOVirtualAddress base;
 uint64_t length;  // mapped BAR0 bytes (bounds the derived GGTT PTE reads)
 ReimsGTWatch::Shared shared;
 bool stop;
 uint32_t exited;
};
static uint64_t uptimeMS(){uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000ULL;}
static void watchExit(GTWatch*w){
 __atomic_fetch_add(&w->exited,1u,__ATOMIC_ACQ_REL);
 thread_terminate(current_thread());
}
// Reads only. Fills the buffer the reporter is not reading, then publishes.
static void watchSampler(void*arg,wait_result_t){
 auto*w=static_cast<GTWatch*>(arg);
 BarIO io{w->base};
 while(!__atomic_load_n(&w->stop,__ATOMIC_ACQUIRE)){
  const uint32_t next=__atomic_load_n(&w->shared.seq,__ATOMIC_RELAXED)+1;
  auto&b=w->shared.buf[next&1];
  for(unsigned i=0;i<ReimsGTSampler::count;++i){
   __atomic_store_n(&w->shared.reading,int32_t(i),__ATOMIC_RELEASE);
   b.v[i]=ReimsGTSampler::readColumn(i,b.v,w->length,[&](uint32_t o){return io.read(o);});
  }
  __atomic_store_n(&w->shared.reading,-1,__ATOMIC_RELEASE);
  __atomic_store_n(&w->shared.seq,next,__ATOMIC_RELEASE);
  IOSleep(250);
 }
 watchExit(w);
}
// Never touches MMIO: prints what the sampler published, or a stall.
static void watchReporter(void*arg,wait_result_t){
 auto*w=static_cast<GTWatch*>(arg);
 ReimsGTWatch::Reporter rep;
 const uint64_t t0=uptimeMS();
 char line[ReimsGTSampler::lineBuffer];
 for(unsigned p=0;p<ReimsGTSampler::parts;++p){ReimsGTSampler::header(line,sizeof(line),int(p));IOLog("%s",line);}
 while(!__atomic_load_n(&w->stop,__ATOMIC_ACQUIRE)){
  IOSleep(250);
  const uint32_t seq=__atomic_load_n(&w->shared.seq,__ATOMIC_ACQUIRE);
  const int32_t reading=__atomic_load_n(&w->shared.reading,__ATOMIC_ACQUIRE);
  const ReimsGTSampler::Sample s=w->shared.buf[seq&1];
  const uint64_t now=uptimeMS()-t0;
  const unsigned out=rep.tick(now,seq,reading,s);
  if(out&ReimsGTWatch::sample)for(unsigned p=0;p<ReimsGTSampler::parts;++p){ReimsGTSampler::format(line,sizeof(line),seq,now,s,int(p));IOLog("%s",line);}
  if(out&ReimsGTWatch::stall){
   ReimsGTWatch::formatStall(line,sizeof(line),seq,now,reading,rep.stuckMS(now));IOLog("%s",line);
  }
 }
 watchExit(w);
}
#endif

class ReimsGTResetProbe:public IOService {
 OSDeclareDefaultStructors(ReimsGTResetProbe)
 IOMemoryMap*heldMap=nullptr;  // hold build: BAR0 stays mapped while forcewake is held
#if defined(REIMS_GT_FORCEWAKE_HOLD)
 GTWatch*watch=nullptr;
 void armWatch(IOMemoryMap*map){
  if(map->getLength()<ReimsGTSampler::maxOffset()+4){setProperty("ReimsGTWatch",false);return;}
  watch=static_cast<GTWatch*>(IOMalloc(sizeof(GTWatch)));
  if(!watch){setProperty("ReimsGTWatch",false);return;}
  bzero(watch,sizeof(*watch));
  watch->base=map->getVirtualAddress();
  watch->length=map->getLength();
  watch->shared.reading=-1;
  thread_t sampler=nullptr,reporter=nullptr;
  if(kernel_thread_start(watchSampler,watch,&sampler)!=KERN_SUCCESS){
   IOFree(watch,sizeof(GTWatch));watch=nullptr;setProperty("ReimsGTWatch",false);return;
  }
  thread_deallocate(sampler);
  if(kernel_thread_start(watchReporter,watch,&reporter)!=KERN_SUCCESS){
   __atomic_store_n(&watch->exited,1u,__ATOMIC_RELEASE);  // count the missing reporter as exited
  } else thread_deallocate(reporter);
  setProperty("ReimsGTWatch",true);
 }
 // True when both threads exited and the BAR may be touched and unmapped.
 bool disarmWatch(){
  if(!watch)return true;
  __atomic_store_n(&watch->stop,true,__ATOMIC_RELEASE);
  for(int i=0;i<300&&__atomic_load_n(&watch->exited,__ATOMIC_ACQUIRE)<2;++i)IOSleep(10);
  if(__atomic_load_n(&watch->exited,__ATOMIC_ACQUIRE)<2){
   IOLog("ReimsGT watch: sampler did not exit (stuck in a register read); leaving BAR0 mapped and the hold set\n");
   watch=nullptr;  // leaked on purpose: a stuck thread still references it
   return false;
  }
  IOFree(watch,sizeof(GTWatch));watch=nullptr;
  return true;
 }
#endif
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
#if defined(REIMS_GT_FORCEWAKE_HOLD)
   armWatch(map);
#endif
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
#if defined(REIMS_GT_FORCEWAKE_HOLD)
  if(!disarmWatch())heldMap=nullptr;  // a stuck read: do not touch or unmap the BAR
#endif
  if(heldMap){
   BarIO io{heldMap->getVirtualAddress()};
   ReimsGTReset::releaseHold(io);
   OSSafeReleaseNULL(heldMap);
  }
  IOService::stop(provider);
 }
};
OSDefineMetaClassAndStructors(ReimsGTResetProbe,IOService)
