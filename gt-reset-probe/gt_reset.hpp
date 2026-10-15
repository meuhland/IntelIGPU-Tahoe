#pragma once
#include <stdint.h>

// Phase-6 GT-reset probe. The window of 2026-10-14 found the render geometry
// fuse mirrors reading 0 on the cold, GOP-less passed-through GT, so Apple's
// getGPUInfo builds a 0-EU GPU and wedges. This tests one lever, read-mostly:
// does a GT reset (GDRST) make the geometry fuses populate? It reads the fuses
// raw, acquires forcewake and reads again, issues GEN6_GDRST, and re-reads.
//
// All register/bit facts are Linux i915 (intel_gt_regs.h, intel_reset.c
// gen6_hw_domain_reset verified 2026-10-14):
//   GEN11_GT_SLICE_ENABLE          0x9138  (should read 0x1 on real Gen12)
//   GEN12_GT_GEOMETRY_DSS_ENABLE   0x913c  (DSS enable mask, should be nonzero)
//   GEN11_EU_DISABLE               0x9134  (disable mask; 0 = all present)
//   GEN11_GT_VEBOX_VDBOX_DISABLE   0x9140  (media; read 0xf00ff in the window)
//   GEN6_GDRST                     0x941c  FULL=bit0 RENDER=bit1; not in the GT
//                                          power well, needs no forcewake
//   FORCEWAKE_RENDER_GEN9 req 0xa278 ack 0xd84
//   FORCEWAKE_GT_GEN9     req 0xa188 ack 0x130044   FORCEWAKE_KERNEL=bit0
//
// Caveat: GDRST is an engine/GT reset, which may not re-trigger the cold-boot
// fuse download; if the fuses stay 0 after reset, the lever is deeper (a PCI
// FLR, or the vfio-pci bind reset clearing the host's fuse download before the
// VM sees the device). The probe answers that empirically.
namespace ReimsGTReset {
namespace R {
constexpr uint32_t sliceEnable=0x9138,dssEnable=0x913c,euDisable=0x9134,mediaDisable=0x9140;
constexpr uint32_t gdrst=0x941c,grdomFull=1u<<0,grdomRender=1u<<1;
constexpr uint32_t fwRenderReq=0xa278,fwRenderAck=0xd84,fwGtReq=0xa188,fwGtAck=0x130044,fwKernel=1u<<0;
// Hold mode's requester bit: i915 uses bit 0 (FORCEWAKE_KERNEL), Apple's
// SafeForceWakeMultithreaded and i915's FORCEWAKE_USER use bit 1. A domain
// stays awake while any requester bit is set, so bit 2 coexists with both.
constexpr uint32_t fwHold=1u<<2;
constexpr uint32_t maxOffset=0x130044;
}

struct Fuses {uint32_t slice=0,dss=0,euDisable=0,media=0;};

struct Result {
 Fuses raw;                 // no forcewake
 Fuses forcewake;           // render+GT forcewake held
 Fuses afterReset;          // forcewake held, after GDRST
 bool renderAcked=false,gtAcked=false;
 bool resetCleared=false;   // GDRST self-cleared within the timeout
 uint32_t resetLoops=0;
 bool executed=false;       // false = dry run (raw only, no writes)
};

// masked write: (mask<<16)|value, the Gen9+ forcewake convention.
template<class IO> inline bool acquireForcewake(IO&io,uint32_t req,uint32_t ack,uint32_t bit=R::fwKernel){
 io.write(req,(bit<<16)|bit);
 for(int i=0;i<100;++i){if(io.read(ack)&bit)return true;io.delayUS(10);}
 return false;
}
template<class IO> inline void releaseForcewake(IO&io,uint32_t req,uint32_t bit=R::fwKernel){
 io.write(req,(bit<<16)|0);
}
template<class IO> inline Fuses readFuses(IO&io){
 return {io.read(R::sliceEnable),io.read(R::dssEnable),io.read(R::euDisable),io.read(R::mediaDisable)};
}

// gen6_hw_domain_reset: write the mask, poll until it self-clears (2 ms),
// repeated twice on pre-MTL Gen12.
template<class IO> inline bool gdrstReset(IO&io,uint32_t mask,uint32_t&loopsOut){
 bool cleared=false;int loops=2;
 do{
  io.write(R::gdrst,mask);
  cleared=false;
  for(int i=0;i<200;++i){if((io.read(R::gdrst)&mask)==0){cleared=true;break;}io.delayUS(10);}
  ++loopsOut;
 }while(cleared&&--loops);
 return cleared;
}

// execute=false: read raw fuses only, no writes. execute=true: raw read,
// forcewake, read, GDRST full reset, read, release forcewake.
template<class IO> inline Result run(IO&io,bool execute){
 Result r;
 r.raw=readFuses(io);
 r.executed=execute;
 if(!execute)return r;
 r.renderAcked=acquireForcewake(io,R::fwRenderReq,R::fwRenderAck);
 r.gtAcked=acquireForcewake(io,R::fwGtReq,R::fwGtAck);
 r.forcewake=readFuses(io);
 r.resetCleared=gdrstReset(io,R::grdomFull,r.resetLoops);
 r.afterReset=readFuses(io);
 releaseForcewake(io,R::fwRenderReq);
 releaseForcewake(io,R::fwGtReq);
 return r;
}

// Hold mode (2026-10-15 diagnosis). Without an AppleIntelFramebufferController
// Apple runs standalone, and its local _SafeForceWake callback is an empty
// stub; getGPUInfo runs in configureDevice before the MultiForceWake hardware
// path is selected, so its forcewake bracket does nothing and it reads the GT
// asleep (all fuses 0, as the host reads right after QEMU's FLR). Holding our
// own requester bit keeps render+GT awake through Apple's start. No reset.
struct Hold {
 Fuses raw;                 // before the hold
 Fuses held;                // render+GT held with fwHold
 bool renderAcked=false,gtAcked=false;
};
template<class IO> inline Hold acquireHold(IO&io){
 Hold h;
 h.raw=readFuses(io);
 h.renderAcked=acquireForcewake(io,R::fwRenderReq,R::fwRenderAck,R::fwHold);
 h.gtAcked=acquireForcewake(io,R::fwGtReq,R::fwGtAck,R::fwHold);
 h.held=readFuses(io);
 return h;
}
template<class IO> inline void releaseHold(IO&io){
 releaseForcewake(io,R::fwRenderReq,R::fwHold);
 releaseForcewake(io,R::fwGtReq,R::fwHold);
}
inline bool populated(const Fuses&f){return (f.slice&0x1)&&f.dss;}

// Did the reset populate the geometry fuses? i915 expects slice-enable 0x1 and
// a nonzero DSS mask; "populated" = either became nonzero after the reset.
inline bool resetPopulatedFuses(const Result&r){
 return r.executed&&
   ((r.afterReset.slice&0x1)>(r.raw.slice&0x1)||r.afterReset.dss>r.raw.dss);
}
}
