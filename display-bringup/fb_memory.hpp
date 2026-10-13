#pragma once
#if !defined(REIMS_FB_HOST_TEST)
#include <IOKit/IOBufferMemoryDescriptor.h>  // at file scope: declares ::IOBufferMemoryDescriptor
#endif

// The 3840x2160 XRGB8888 framebuffer plane 1A scans out, shared
// by ReimsDisplayBringup (test pattern) and DesktopLink's native light (black,
// docs/HANDOFF-OPTIONS-RPLS.md option C). Display reads are not snooped, so
// the contents are flushed from the CPU caches. Pages are below 512 GiB; the
// guest has no virtual IOMMU, so guest-physical addresses are DMA addresses.
namespace ReimsBringup {
enum class Fill {TestPattern,Black};
// A 16-pixel white border around eight colour bars, with a grey ramp in the
// bottom quarter.
inline void fillTestPattern(uint32_t*px){
 static const uint32_t bars[8]={0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0x000000};
 for(uint32_t y=0;y<2160;++y)for(uint32_t x=0;x<3840;++x){
  uint32_t c;
  if(x<16||y<16||x>=3824||y>=2144)c=0xffffff;
  else if(y>=1620){const uint32_t g=(x-16)*255/3807;c=g<<16|g<<8|g;}
  else c=bars[(x-16)*8/3808];
  px[y*3840+x]=c;
 }
}
inline void fillBlack(uint32_t*px){for(uint32_t i=0;i<3840u*2160u;++i)px[i]=0;}
#if !defined(REIMS_FB_HOST_TEST)
// Allocates, fills, flushes and prepares `pages` pages and returns each page's
// DMA address in dma[]. The caller completes and releases the descriptor once
// no plane can fetch from it.
inline IOBufferMemoryDescriptor*makeFramebuffer(uint64_t*dma,uint32_t pages,Fill fill){
 auto*m=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,kIODirectionInOut,
  uint64_t(pages)*4096,0x0000007ffffff000ULL);
 if(!m)return nullptr;
 if(m->prepare()!=kIOReturnSuccess){m->release();return nullptr;}
 auto*px=static_cast<uint32_t*>(m->getBytesNoCopy());
 if(fill==Fill::TestPattern)fillTestPattern(px);
 else fillBlack(px);
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
#endif // !REIMS_FB_HOST_TEST
}
