#pragma once
// Option C (docs/HANDOFF-OPTIONS-RPLS.md): DesktopLink's native framebuffer
// lights the display itself at commit, after PrepareRuntime, instead of
// inheriting a pipe the separate bring-up kext lit (which the accelerator
// blanks). It reuses the host-tested stage 2/3 sequence over DesktopLink's
// own BAR0 mapping and its own black framebuffer.
//
// Opt-in and unfinished: only compiled when REIMS_RPLS_SELF_LIGHT is defined,
// so the committed rpls build is unchanged until this path is reviewed with
// kHardwareWritesPorted. It writes display registers; it is one display owner
// from commit onward, so it must not run while the bring-up kext still owns a
// lit plane.
#if defined(REIMS_RPLS_SELF_LIGHT)
#if !defined(REIMS_TARGET_RPLS)
#error "REIMS_RPLS_SELF_LIGHT is RPL-S only"
#endif
// The bring-up headers write RPL-S registers; declare the same build intent.
#define REIMS_DISPLAY_BRINGUP 1
#define REIMS_BRINGUP_EXECUTE 1
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include "../../../display-bringup/hdmi_scanout.hpp"
#include "../../../display-bringup/fb_memory.hpp"

namespace ReimsLight {
// Adapts DesktopLink's BAR0 IOMemoryMap to the bring-up headers' IO interface
// (read/write/write64/delayUS/nowUS). Execute only; no dry-run counting.
struct BarIO {
 IOVirtualAddress base;
 uint32_t read(uint32_t o){return *reinterpret_cast<volatile uint32_t*>(base+o);}
 void write(uint32_t o,uint32_t v){*reinterpret_cast<volatile uint32_t*>(base+o)=v;(void)read(o);}
 void write64(uint32_t o,uint64_t v){*reinterpret_cast<volatile uint64_t*>(base+o)=v;}
 void delayUS(uint32_t us){IODelay(us);}
 uint64_t nowUS(){uint64_t a=0,ns=0;clock_get_uptime(&a);absolutetime_to_nanoseconds(a,&ns);return ns/1000;}
};

// Lights pipe A on HDMI TC1 at 3840x2160@30 scanning a black framebuffer at
// GGTT 0x01000000. On success *fb owns the wired pages (freed by the caller
// once no plane can fetch them) and the plane is live. Returns OK/Planned from
// the scanout, or an error; frees the framebuffer on any failure.
inline ReimsBringup::Result light(IOVirtualAddress bar,IOBufferMemoryDescriptor**fb){
 *fb=nullptr;
 constexpr uint32_t pages=ReimsBringup::R4::pages,ggtt=0x01000000;
 auto*dma=static_cast<uint64_t*>(IOMalloc(pages*sizeof(uint64_t)));
 if(!dma)return ReimsBringup::Result::PreconditionFailed;
 auto*mem=ReimsBringup::makeFramebuffer(dma,pages,ReimsBringup::Fill::Black);
 if(!mem){IOFree(dma,pages*sizeof(uint64_t));return ReimsBringup::Result::PreconditionFailed;}
 BarIO io{bar};
 ReimsBringup::HdmiScanout<BarIO> seq(io,true);
 const auto r=seq.runScanout(ReimsBringup::kUhd30,{ggtt,pages,dma});
 IOFree(dma,pages*sizeof(uint64_t));
 if(r!=ReimsBringup::Result::OK){mem->complete();mem->release();return r;}
 *fb=mem;return r;
}
}
#endif
