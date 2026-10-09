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
#if !defined(REIMS_BRINGUP_STAGE)
#define REIMS_BRINGUP_STAGE 2
#endif
#if REIMS_BRINGUP_STAGE==3
#include "hdmi_scanout.hpp"
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
// Unloading disables the plane before freeing its framebuffer; the HDMI link
// stays up showing black.
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
 void refuse(const char*why){setProperty("ReimsBringupError",why);}
 // Only the boot framebuffer may be attached to the IGD.
 bool onlyBootFramebuffer(IOPCIDevice*pci){
  auto*it=pci->getChildIterator(gIOServicePlane);if(!it)return false;
  bool ok=true;
  while(auto*child=OSDynamicCast(IORegistryEntry,it->getNextObject()))
   ok&=!strcmp(child->getMetaClass()->getClassName(),"IONDRVFramebuffer");
  it->release();return ok;
 }
#if REIMS_BRINGUP_STAGE==3
 // 3840x2160 XRGB8888: a 16-pixel white border around eight colour bars,
 // with a grey ramp in the bottom quarter. Display reads are not snooped, so
 // the pattern is flushed from the CPU caches. Pages are below 512 GiB; the
 // guest has no virtual IOMMU, so guest-physical addresses are DMA addresses.
 IOBufferMemoryDescriptor*makeFramebuffer(uint64_t*dma,uint32_t pages){
  auto*m=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,kIODirectionInOut,
   uint64_t(pages)*4096,0x0000007ffffff000ULL);
  if(!m)return nullptr;
  if(m->prepare()!=kIOReturnSuccess){m->release();return nullptr;}
  auto*px=static_cast<uint32_t*>(m->getBytesNoCopy());
  static const uint32_t bars[8]={0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0x000000};
  for(uint32_t y=0;y<2160;++y)for(uint32_t x=0;x<3840;++x){
   uint32_t c;
   if(x<16||y<16||x>=3824||y>=2144)c=0xffffff;
   else if(y>=1620){const uint32_t g=(x-16)*255/3807;c=g<<16|g<<8|g;}
   else c=bars[(x-16)*8/3808];
   px[y*3840+x]=c;
  }
  const auto base=reinterpret_cast<uintptr_t>(px);
  for(uint64_t off=0;off<uint64_t(pages)*4096;off+=64)
   __asm__ volatile("clflush (%0)"::"r"(base+off):"memory");
  __asm__ volatile("mfence":::"memory");
  for(uint32_t i=0;i<pages;++i){
   IOByteCount len=0;
   dma[i]=m->getPhysicalSegment(uint64_t(i)*4096,&len,kIOMemoryMapperNone);
   if(!dma[i]||len<4096){m->complete();m->release();return nullptr;}
  }
  return m;
 }
#endif
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
  entry->release();
  if(!bar||bar->getLength()<barNeeded){OSSafeReleaseNULL(bar);refuse("BAR0 map failed");return false;}
  BarIO io{bar->getVirtualAddress()};
#if REIMS_BRINGUP_STAGE==3
  constexpr uint32_t pages=ReimsBringup::R4::pages,ggtt=0x01000000;
  auto*dma=static_cast<uint64_t*>(IOMalloc(pages*sizeof(uint64_t)));
  if(!dma){refuse("out of memory");return false;}
  fbMemory=makeFramebuffer(dma,pages);
  if(!fbMemory){IOFree(dma,pages*sizeof(uint64_t));refuse("framebuffer allocation failed");return false;}
  setProperty("ReimsBringupFramebufferGGTT",uint64_t(ggtt),32);
  setProperty("ReimsBringupFramebufferFirstDMA",dma[0],64);
  ReimsBringup::HdmiScanout<BarIO> init(io,kExecute);
  const auto result=init.runScanout(ReimsBringup::kUhd30,{ggtt,pages,dma});
  IOFree(dma,pages*sizeof(uint64_t));
  planeOn=kExecute&&(io.read(ReimsBringup::R4::planeCtl)&(1U<<31));
  if(!planeOn){fbMemory->complete();OSSafeReleaseNULL(fbMemory);}
  setProperty("ReimsBringupPlaneOn",planeOn);
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
  // Stay attached, even after a refusal, so the result can be read back.
  registerService();
  return true;
 }
 void stop(IOService*provider)override{
#if REIMS_BRINGUP_STAGE==3 && defined(REIMS_BRINGUP_EXECUTE)
  // skl_plane_disable_arm, then let it latch before freeing the pages.
  if(planeOn&&bar){
   BarIO io{bar->getVirtualAddress()};
   io.write(ReimsBringup::R4::planeCtl,0);io.write(ReimsBringup::R4::planeSurf,0);
   IOSleep(100);planeOn=false;
  }
#endif
  if(fbMemory){fbMemory->complete();OSSafeReleaseNULL(fbMemory);}
  OSSafeReleaseNULL(bar);
  IOService::stop(provider);
 }
};
OSDefineMetaClassAndStructors(ReimsDisplayBringup,IOService)
