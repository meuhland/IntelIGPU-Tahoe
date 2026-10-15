#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSIterator.h>
#include "../common/reims_target.hpp"
#if defined(REIMS_GT_SAMPLER)
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include "gt_sampler.hpp"
#endif
#if !defined(REIMS_BRINGUP_STAGE)
#define REIMS_BRINGUP_STAGE 2
#endif
#if REIMS_BRINGUP_STAGE==3
#include "hdmi_scanout.hpp"
#include "fb_memory.hpp"
#include "edid_read.hpp"
#elif REIMS_BRINGUP_STAGE==2
#include "core_init.hpp"
#else
#error "REIMS_BRINGUP_STAGE must be 2 or 3"
#endif

// Phase 5 display bring-up: stage 2 (display core init) or, with
// REIMS_BRINGUP_STAGE=3, stage 2 + 3a (HDMI TC1 at 3840x2160@30) + 3b
// (plane 1A scanning out a test pattern; hdmi_scanout.hpp). Builds only with
// REIMS_DISPLAY_BRINGUP=1 and REIMS_TARGET=rpls. Without REIMS_BRINGUP_EXECUTE
// it is a dry run: reads only, and publishes the writes it would make.
// Refuses unless the IGD is isolated (registry device-id FFFF), carries no
// driver other than the boot IONDRVFramebuffer, and decodes memory.
// Stage 3 scans out from the first unused GGTT range from 16 MiB up inside
// GMADR (published as ReimsBringupFramebufferGGTT). Unloading disables the
// plane, restores that range's GGTT entries, then frees the framebuffer; the
// HDMI link stays up showing black.
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
 void write64(uint32_t o,uint64_t v){*reinterpret_cast<volatile uint64_t*>(base+o)=v;}
#else
 void write(uint32_t,uint32_t){++blockedWrites;}
 void write64(uint32_t,uint64_t){++blockedWrites;}
#endif
 void delayUS(uint32_t us){IODelay(us);}
 uint64_t nowUS(){uint64_t a=0,ns=0;clock_get_uptime(&a);absolutetime_to_nanoseconds(a,&ns);return ns/1000;}
};

class ReimsDisplayBringup:public IOService {
 OSDeclareDefaultStructors(ReimsDisplayBringup)
 IOMemoryMap*bar=nullptr;IOBufferMemoryDescriptor*fbMemory=nullptr;bool planeOn=false;
 // The framebuffer's GGTT range and the entry it held before (restored on stop).
 uint32_t ggttOffset=0;uint64_t ggttPrior=0;bool ggttWritten=false;
 void refuse(const char*why){setProperty("ReimsBringupError",why);}
#if defined(REIMS_GT_SAMPLER)
 // Read-only GT watch (gt_sampler.hpp): every 250 ms while loaded, print the
 // sampled registers with IOLog (serial console) when they change, plus a
 // heartbeat. Its own work loop, so a wedged accelerator gate or IOKit lock
 // does not stop it; it reads through the BAR0 mapping and never writes.
 IOWorkLoop*samplerLoop=nullptr;IOTimerEventSource*samplerTimer=nullptr;
 ReimsGTSampler::State samplerState;uint32_t samplerSeq=0;uint64_t samplerStartUS=0;
 static uint64_t uptimeUS(){uint64_t a=0,ns=0;clock_get_uptime(&a);absolutetime_to_nanoseconds(a,&ns);return ns/1000;}
 static void sampleTick(OSObject*owner,IOTimerEventSource*timer){
  auto*self=OSDynamicCast(ReimsDisplayBringup,owner);if(!self||!self->bar)return;
  BarIO io{self->bar->getVirtualAddress()};
  ReimsGTSampler::Sample sample;
  for(unsigned i=0;i<ReimsGTSampler::count;++i)sample.v[i]=io.read(ReimsGTSampler::regs[i].offset);
  if(self->samplerState.shouldPrint(sample,ReimsGTSampler::Policy{})){
   char line[ReimsGTSampler::lineBuffer];
   const uint64_t ms=(uptimeUS()-self->samplerStartUS)/1000;
   for(unsigned p=0;p<ReimsGTSampler::parts;++p){
    ReimsGTSampler::format(line,sizeof(line),self->samplerSeq,ms,sample,int(p));IOLog("%s",line);
   }
  }
  ++self->samplerSeq;timer->setTimeoutMS(250);
 }
 void armSampler(){
  if(!bar||bar->getLength()<ReimsGTSampler::maxOffset()+4){setProperty("ReimsGTSampler",false);return;}
  samplerLoop=IOWorkLoop::workLoop();
  samplerTimer=samplerLoop?IOTimerEventSource::timerEventSource(this,sampleTick):nullptr;
  if(!samplerTimer||samplerLoop->addEventSource(samplerTimer)!=kIOReturnSuccess){
   OSSafeReleaseNULL(samplerTimer);OSSafeReleaseNULL(samplerLoop);setProperty("ReimsGTSampler",false);return;
  }
  char line[ReimsGTSampler::lineBuffer];for(unsigned p=0;p<ReimsGTSampler::parts;++p){ReimsGTSampler::header(line,sizeof(line),int(p));IOLog("%s",line);}
  samplerStartUS=uptimeUS();samplerTimer->setTimeoutMS(250);setProperty("ReimsGTSampler",true);
 }
 void disarmSampler(){
  if(samplerTimer){samplerTimer->cancelTimeout();samplerLoop->removeEventSource(samplerTimer);OSSafeReleaseNULL(samplerTimer);}
  OSSafeReleaseNULL(samplerLoop);
 }
#endif
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
  // Stage 3 writes GGTT entries in the upper half of the 16 MiB BAR0.
  constexpr IOByteCount barNeeded=REIMS_BRINGUP_STAGE==3?0x1000000:0x200000;
  bar=pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
#if REIMS_BRINGUP_STAGE==3
  // The native framebuffer later reaches the plane's pages through GMADR
  // (BAR2), so the framebuffer's GGTT range must lie inside it.
  uint64_t gmadrLength=0;
  if(auto*gmadr=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2))gmadrLength=gmadr->getLength();
#endif
  entry->release();
  if(!bar||bar->getLength()<barNeeded){OSSafeReleaseNULL(bar);refuse("BAR0 map failed");return false;}
  BarIO io{bar->getVirtualAddress()};
#if REIMS_BRINGUP_STAGE==3
  constexpr uint32_t pages=ReimsBringup::R4::pages;
  // Use the first unused GGTT range from 16 MiB up inside GMADR. The fixed
  // 0x01000000 used until 2026-10-14 was already taken on the boots that day
  // (possibly by entries an earlier run left: stop() did not restore them).
  setProperty("ReimsBringupGMADRLength",gmadrLength,64);
  const uint32_t ggtt=ReimsBringup::findUnusedGgtt(io,ReimsBringup::R4::firstGgttCandidate,gmadrLength,pages);
  if(!ggtt){refuse("no unused GGTT range inside the GMADR aperture");return false;}
  auto*dma=static_cast<uint64_t*>(IOMalloc(pages*sizeof(uint64_t)));
  if(!dma){refuse("out of memory");return false;}
  fbMemory=ReimsBringup::makeFramebuffer(dma,pages,ReimsBringup::Fill::TestPattern);
  if(!fbMemory){IOFree(dma,pages*sizeof(uint64_t));refuse("framebuffer allocation failed");return false;}
  setProperty("ReimsBringupFramebufferGGTT",uint64_t(ggtt),32);
  setProperty("ReimsBringupFramebufferFirstDMA",dma[0],64);
  ReimsBringup::HdmiScanout<BarIO> init(io,kExecute);
#if defined(REIMS_BRINGUP_UHD60)
  // 4K60: 594 MHz TMDS, HDMI 2.0 SCDC scrambling (hdmi_scdc.hpp) and the
  // DDI scrambling bits; plane DDB/watermarks are host i915's 4K60 values.
  constexpr const ReimsBringup::HdmiMode&mode=ReimsBringup::kUhd60;
#else
  constexpr const ReimsBringup::HdmiMode&mode=ReimsBringup::kUhd30;
#endif
  setProperty("ReimsBringupRefreshHz",uint64_t(mode.refreshHz),32);
  const auto result=init.runScanout(mode,{ggtt,pages,dma});
  IOFree(dma,pages*sizeof(uint64_t));
  planeOn=kExecute&&(io.read(ReimsBringup::R4::planeCtl)&(1U<<31));
  if(!planeOn){
   // Nothing scans out: put the GGTT entries back before freeing the pages.
   init.restoreGgtt({ggtt,pages,nullptr});
   fbMemory->complete();OSSafeReleaseNULL(fbMemory);
  }else{ggttOffset=ggtt;ggttPrior=init.ggttPrior;ggttWritten=init.ggttWritten;}
  setProperty("ReimsBringupPlaneOn",planeOn);
  // Read the monitor's EDID over GMBUS (VBT DDC pin 2 = HDMI-B, which is GMBUS
  // pin 9 on ADL-S; the 2026-10-13 run used pin 2 and timed out). This
  // drives the PCH I2C engine via GMBUS control-register writes, so it only
  // runs in the execute build; the dry run cannot move the bus.
  if(kExecute){
   ReimsEDID::Reader<BarIO> ddc(io);ReimsEDID::Edid e;
   const auto es=ddc.read(ReimsEDID::adlsGmbusPin(ReimsEDID::kHdmiBVbtDdcPin),e);
   setProperty("ReimsEDIDStatus",uint64_t(es),32);
   if(es==ReimsEDID::Status::OK){
    setProperty("ReimsEDIDVendor",e.vendor);
    setProperty("ReimsEDIDProduct",uint64_t(e.product),32);
    setProperty("ReimsEDIDVersion",uint64_t(e.version)<<8|e.revision,32);
    setProperty("ReimsEDIDPreferredHActive",uint64_t(e.prefHActive),32);
    setProperty("ReimsEDIDPreferredVActive",uint64_t(e.prefVActive),32);
    setProperty("ReimsEDIDPreferredPixelClockKHz",uint64_t(e.prefPixelClockKHz),32);
    setProperty("ReimsEDIDExtensions",uint64_t(e.extensions),32);
    if(auto*d=OSData::withBytes(e.raw,sizeof(e.raw))){setProperty("ReimsEDIDBlock0",d);d->release();}
   }
  }
#else
  ReimsBringup::CoreInit<BarIO> init(io,kExecute);
  const auto result=init.run();
#endif
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
#if defined(REIMS_GT_SAMPLER)
  armSampler();
#endif
  // Stay attached, even after a refusal, so the result can be read back.
  registerService();
  return true;
 }
 void stop(IOService*provider)override{
#if defined(REIMS_GT_SAMPLER)
  disarmSampler();
#endif
#if REIMS_BRINGUP_STAGE==3 && defined(REIMS_BRINGUP_EXECUTE)
  // skl_plane_disable_arm, then let it latch before freeing the pages.
  if(planeOn&&bar){
   BarIO io{bar->getVirtualAddress()};
   io.write(ReimsBringup::R4::planeCtl,0);io.write(ReimsBringup::R4::planeSurf,0);
   IOSleep(100);planeOn=false;
   // The plane is off: point the range back at its original entry, so no GGTT
   // entry is left pointing at the pages freed below.
   if(ggttWritten)ReimsBringup::restoreGgtt(io,ggttOffset,ReimsBringup::R4::pages,ggttPrior);
   ggttWritten=false;
  }
#endif
  if(fbMemory){fbMemory->complete();OSSafeReleaseNULL(fbMemory);}
  OSSafeReleaseNULL(bar);
  IOService::stop(provider);
 }
};
OSDefineMetaClassAndStructors(ReimsDisplayBringup,IOService)
