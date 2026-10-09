#pragma once
#include "hdmi_output.hpp"

// Phase 5 stage 3b: scan out a framebuffer on plane 1A over the stage 3a
// HDMI link. Ported from Linux i915 v7.2: gen9_dbuf_slices_update (DBUF S2),
// icl_color_commit_arm (no CSC/gamma: full-range RGB), gen8_ggtt_insert_page
// (GGTT at BAR0 + 8 MiB, PTE = DMA address | present), icl_plane_update_noarm
// (+ skl_write_plane_wm) and icl_plane_update_arm (PLANE_CTL, PLANE_SURF).
// Plane values are host i915's for the same 3840x2160 XRGB8888 linear
// framebuffer; its DDB allocation and watermarks were computed for 4K60 and
// are conservative at 4K30.
// IO additionally provides void write64(uint32_t, uint64_t) for GGTT entries.
namespace ReimsBringup {
struct Framebuffer {
 uint32_t ggttOffset;       // GGTT address of the first page (256 KiB aligned)
 uint32_t pages;            // 4 KiB pages
 const uint64_t*dma;        // DMA address of each page
};
namespace R4 {
constexpr uint32_t dbufS2=0x44fe8;                   // DBUF_CTL_S(DBUF_S2)
constexpr uint32_t gammaModeA=0x4a480,cscModeA=0x49028;
constexpr uint32_t ggttBase=0x800000;                // gen6_gttadr_offset: BAR0 size / 2
constexpr uint64_t ptePresent=1;                     // GEN8_PAGE_PRESENT
constexpr uint32_t planeCtl=0x70180,planeStride=0x70188,planePos=0x7018c,planeSize=0x70190,
 planeKeyval=0x70194,planeKeymsk=0x70198,planeSurf=0x7019c,planeKeymax=0x701a0,
 planeOffset=0x701a4,planeAuxDist=0x701c0,planeCusCtl=0x701c8,planeColorCtl=0x701cc,
 planeSurfLive=0x701ac,planeWm0=0x70240,planeWmTrans=0x70268,planeBufCfg=0x7027c;
constexpr uint32_t pipeIirA=0x44408,fifoUnderrun=1U<<31; // GEN8_DE_PIPE_IIR(A)
// Host i915, plane 1A, 3840x2160 XRGB8888 linear.
constexpr uint32_t ctlValue=1U<<31|4U<<24;           // PLANE_CTL_ENABLE | FORMAT_XRGB_8888
constexpr uint32_t strideValue=3840*4/64;            // 64-byte units: 0xf0
constexpr uint32_t sizeValue=(2160-1)<<16|(3840-1);
constexpr uint32_t keymaxValue=0xffU<<24;            // plane alpha 0xff
constexpr uint32_t colorCtlValue=1U<<13;             // PLANE_COLOR_PLANE_GAMMA_DISABLE
constexpr uint32_t wm[8]={0x8000401f,0x8000c05e,0x8001409c,0x8001409c,
 0x8001409c,0x8001c0da,0x800381b3,0x8005026d};
constexpr uint32_t wmTrans=0x8000002d;
constexpr uint32_t bufCfg=0x07ba0000;                 // DDB blocks 0-1978 (i915_ddb_info)
constexpr uint32_t pages=3840*4*2160/4096;            // 8100
}

template<class IO> class HdmiScanout:public HdmiOutput<IO> {
 using CoreInit<IO>::io;using CoreInit<IO>::execute;using CoreInit<IO>::record;
 using CoreInit<IO>::rmw;using CoreInit<IO>::write;using CoreInit<IO>::fail;using CoreInit<IO>::waitFor;
public:
 using HdmiOutput<IO>::HdmiOutput;
 Result runScanout(const HdmiMode&m,const Framebuffer&fb){
  // Validate the request and the plane before anything is written.
  record(Step::Scanout,Action::Check,R4::planeSurf,fb.ggttOffset,fb.pages);
  if(m.hActive!=3840||m.vActive!=2160||fb.pages!=R4::pages||(fb.ggttOffset&0x3ffff)||!fb.dma)
   return fail(Step::Scanout,Result::PreconditionFailed,R4::planeSurf,fb.ggttOffset);
  if(io.read(R4::planeCtl)&(1U<<31))
   return fail(Step::Scanout,Result::PreconditionFailed,R4::planeCtl,io.read(R4::planeCtl));
  for(uint32_t i=0;i<fb.pages;++i)
   if(!fb.dma[i]||(fb.dma[i]&0xfff))return fail(Step::Scanout,Result::PreconditionFailed,i,uint32_t(fb.dma[i]));
  Result r=HdmiOutput<IO>::runOutput(m);
  if(r!=(execute?Result::OK:Result::Planned))return r;
  // gen9_dbuf_slices_update: the plane's DDB spans both slices.
  r=rmw(Step::DbufSlice2,R4::dbufS2,0,R::dbufRequest);
  if(r==Result::OK&&execute){
   io.delayUS(10);
   const uint32_t v=io.read(R4::dbufS2);
   if(!(v&R::dbufState))return fail(Step::DbufSlice2,Result::Timeout,R4::dbufS2,v);
  }
  // No CTM, no LUT, full range: GAMMA_MODE 8-bit bypass, CSC_MODE off.
  if(r==Result::OK)r=write(Step::PipeColor,R4::gammaModeA,0,0,true);
  if(r==Result::OK)r=write(Step::PipeColor,R4::cscModeA,0,1,true);
  // GGTT entries (writeq), then a readback of the first and last.
  if(r==Result::OK){
   const uint32_t first=R4::ggttBase+(fb.ggttOffset>>12)*8;
   if(execute){
    for(uint32_t i=0;i<fb.pages;++i)io.write64(first+i*8,fb.dma[i]|R4::ptePresent);
    const uint32_t lo0=io.read(first),loN=io.read(first+(fb.pages-1)*8);
    const bool ok=lo0==uint32_t(fb.dma[0]|R4::ptePresent)&&loN==uint32_t(fb.dma[fb.pages-1]|R4::ptePresent);
    record(Step::Ggtt,ok?Action::Write:Action::Fail,first,lo0,loN);
    if(!ok)return Result::Unexpected;
   }else record(Step::Ggtt,Action::Plan,first,fb.pages,uint32_t(fb.dma[0]|R4::ptePresent));
  }
  // icl_plane_update_noarm.
  const uint32_t noarm[][2]={{R4::planeStride,R4::strideValue},{R4::planePos,0},
   {R4::planeSize,R4::sizeValue},{R4::planeKeyval,0},{R4::planeKeymsk,0},
   {R4::planeKeymax,R4::keymaxValue},{R4::planeOffset,0},{R4::planeAuxDist,0},
   {R4::planeCusCtl,0},{R4::planeColorCtl,R4::colorCtlValue}};
  for(unsigned i=0;i<sizeof(noarm)/sizeof(noarm[0])&&r==Result::OK;++i)
   r=write(Step::PlaneConfig,noarm[i][0],noarm[i][1],uint8_t(i),true);
  // skl_write_plane_wm: levels, transition, DDB.
  for(unsigned l=0;l<8&&r==Result::OK;++l)r=write(Step::PlaneWm,R4::planeWm0+4*l,R4::wm[l],uint8_t(l),true);
  if(r==Result::OK)r=write(Step::PlaneWm,R4::planeWmTrans,R4::wmTrans,8,true);
  if(r==Result::OK)r=write(Step::PlaneWm,R4::planeBufCfg,R4::bufCfg,9,true);
  const uint32_t iirBefore=io.read(R4::pipeIirA);
  // icl_plane_update_arm: PLANE_CTL just before PLANE_SURF.
  if(r==Result::OK)r=write(Step::PlaneEnable,R4::planeCtl,R4::ctlValue);
  if(r==Result::OK)r=write(Step::PlaneEnable,R4::planeSurf,fb.ggttOffset,1);
  if(r!=Result::OK||!execute)return r==Result::OK?Result::Planned:r;
  // The update latches at the next vblank (33 ms at 30 Hz).
  const bool live=waitFor(R4::planeSurfLive,0xfffff000,fb.ggttOffset,100000);
  record(Step::PlaneVerify,live?Action::Check:Action::Fail,R4::planeSurfLive,fb.ggttOffset,io.read(R4::planeSurfLive));
  if(!live)return Result::Timeout;
  io.delayUS(100000);
  record(Step::PlaneVerify,Action::Check,R4::pipeIirA,iirBefore,io.read(R4::pipeIirA),1); // FIFO underrun: bit 31
  record(Step::Done,Action::Check,0,0,0);
  return Result::OK;
 }
 // skl_plane_disable_arm.
 void disablePlane(){if(execute){io.write(R4::planeCtl,0);io.write(R4::planeSurf,0);}}
};
}
