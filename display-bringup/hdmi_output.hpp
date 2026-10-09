#pragma once
#include "core_init.hpp"
#include "hdmi_pll.hpp"
#include "avi_infoframe.hpp"
#include "hdmi_scdc.hpp"
#include "../desktop-reset-recovery-20260917/source/desktop-link/display_timing.hpp"

// Phase 5 stage 3a: drive one HDMI output with no plane (the pipe sends its
// black background), ported from Linux i915 v7.2 hsw_crtc_enable for an
// ADL-S/RPL-S HDMI encoder on a combo PHY. Target: pipe/transcoder A, DDI TC1
// (port 3) on combo PHY B, DPLL0 - the routing host i915 uses on this board.
// Only modes up to 340 MHz are accepted (no HDMI 2.0 scrambling or SCDC).
// Order: power wells 2/3, DPLL0, DDI clock and IO power, transcoder clock,
// AVI infoframe, pipe size/misc, transcoder timings and config, linetime,
// pipe chicken, MBUS DBOX, DDI function, transcoder enable, PHY signal levels
// (VBT HDMI level 6), lanes, DDI buffer; then a decoder readback.
namespace ReimsBringup {
struct HdmiMode {uint32_t hActive,hSyncStart,hSyncEnd,hTotal,vActive,vSyncStart,vSyncEnd,vTotal,clockKHz,refreshHz;bool hPos,vPos,scramble;};
// CEA 3840x2160@30 (297 MHz): the same timings host i915 runs at 60 Hz.
constexpr HdmiMode kUhd30={3840,4016,4104,4400,2160,2168,2178,2250,297000,30,true,true,false};
// 3840x2160@60 (594 MHz > 340): HDMI 2.0, needs SCDC scrambling at 1/40.
constexpr HdmiMode kUhd60={3840,4016,4104,4400,2160,2168,2178,2250,594000,60,true,true,true};

namespace R3 {
constexpr uint32_t pwrWellDriver=0x45404;           // HSW_PWR_WELL_CTL2
constexpr uint32_t pw2Request=1U<<3,pw2State=1U<<2;  // ICL_PW_CTL_IDX_PW_2 (1)
constexpr uint32_t pw3Request=1U<<5,pw3State=1U<<4;  // ICL_PW_CTL_IDX_PW_3 (2)
constexpr uint32_t fusePG2=1U<<25,fusePG3=1U<<24;
constexpr uint32_t ddiPwrDriver=0x45454;            // ICL_PWR_WELL_CTL_DDI2
constexpr uint32_t tc1IoRequest=1U<<7,tc1IoState=1U<<6; // TGL_PW_CTL_IDX_DDI_TC1 (3)
constexpr uint32_t dpll0Enable=0x46010,dpll0Cfgcr0=0x164284,dpll0Cfgcr1=0x164288;
constexpr uint32_t pllEnable=1U<<31,pllLock=1U<<30,pllPowerEnable=1U<<27,pllPowerState=1U<<26;
constexpr uint32_t dpclka0=0x164280;                 // ADLS_DPCLKA_CFGCR(PHY B)
constexpr uint32_t phyBClkSelMask=3U<<2,phyBClkOff=1U<<11;
constexpr uint32_t transClkSelA=0x46140,transClkTc1=4U<<28; // TGL_TRANS_CLK_SEL_PORT(3)
constexpr uint32_t dipCtlA=0x60200;                  // HSW_VIDEO_DIP_CTL(A)
constexpr uint32_t dipAviDataA=0x60220;              // HSW_TVIDEO_DIP_AVI_DATA(A)
constexpr uint32_t dipEnableAvi=1U<<12;              // VIDEO_DIP_ENABLE_AVI_HSW
constexpr uint32_t pipeSrcA=0x6001c,pipeMiscA=0x70030,pipeChickenA=0x70038,mbusDboxA=0x7003c;
constexpr uint32_t pipeMiscValue=1U<<23|1U<<8;      // HDR precision | rounding trunc | BPC_8
constexpr uint32_t pipeChickenBits=1U<<15|1U<<7;    // PIXEL_ROUNDING_TRUNC_FB_PASSTHRU | PER_PIXEL_ALPHA_BYPASS_EN
// pipe_mbus_dbox_ctl (display 12, not ADL-P): B2B max 16, delay 1, regulate,
// BW credit 2, B credit 12, A credit 2.
constexpr uint32_t mbusDboxValue=16U<<20|1U<<17|1U<<16|2U<<14|12U<<8|2U;
constexpr uint32_t mbusDboxMask=0x1fU<<20|7U<<17|1U<<16|3U<<14|0x1fU<<8|0xfU;
constexpr uint32_t htotalA=0x60000,hblankA=0x60004,hsyncA=0x60008,vtotalA=0x6000c,
 vblankA=0x60010,vsyncA=0x60014,vsyncShiftA=0x60028,transMultA=0x6002c;
constexpr uint32_t chickenTransA=0x420c0,frameStartDelayMask=3U<<27;
constexpr uint32_t transConfA=0x70008,transEnable=1U<<31,transState=1U<<30;
constexpr uint32_t linetimeA=0x45270;                // WM_LINETIME(A)
constexpr uint32_t ddiFuncA=0x60400,ddiFunc2A=0x60404;
constexpr uint32_t pipeDslA=0x70000;
constexpr uint32_t ddiBufTc1=0x64300,ddiBufEnable=1U<<31,ddiBufIdle=1U<<7;
// Combo PHY B (0x6c000) lane registers (intel_combo_phy_regs.h).
constexpr uint32_t phyB=0x6c000;
constexpr uint32_t clDw5=phyB+0x14,clDw10=phyB+0x28,pcsDw1Grp=phyB+0x604,pcsDw1Ln0=phyB+0x804;
constexpr uint32_t txDw5Grp=phyB+0x694;
constexpr uint32_t txLane(unsigned ln,unsigned dw){return phyB+0x880+ln*0x100+4*dw;}
constexpr uint32_t commonKeeper=1U<<26,susClockConfig=3U,loadgenSelect=1U<<31;
constexpr uint32_t txTrainingEn=1U<<31,tap2Disable=1U<<30,tap3Disable=1U<<29,
 cursorProgram=1U<<26,coeffPolarity=1U<<25,scalingModeMask=7U<<18,rtermMask=7U<<3;
constexpr uint32_t pwrDownLanesMask=0xfU<<4;
}
// icl_combo_phy_trans_hdmi[6] (VBT HDMI level shifter 6 on this board):
// swing 0x6, n-scalar 0x7f, cursor coeff 0x35, post cursor 2 0, post cursor 1 0xa.
constexpr uint32_t kSwing=0x6,kNScalar=0x7f,kCursorCoeff=0x35,kPostCursor2=0,kPostCursor1=0xa;

template<class IO> class HdmiOutput:public CoreInit<IO> {
 using CoreInit<IO>::io;using CoreInit<IO>::execute;using CoreInit<IO>::record;
 using CoreInit<IO>::rmw;using CoreInit<IO>::write;using CoreInit<IO>::fail;using CoreInit<IO>::waitFor;
 Result waitBits(Step s,uint32_t reg,uint32_t mask,uint32_t value,uint32_t timeoutUS){
  if(!execute||waitFor(reg,mask,value,timeoutUS))return Result::OK;
  return fail(s,Result::Timeout,reg,io.read(reg));
 }
 // hsw_power_well_enable for a well with fuses.
 Result powerWell(Step s,uint32_t request,uint32_t state,uint32_t fuse){
  const uint32_t ctl=io.read(R3::pwrWellDriver);
  if((ctl&(request|state))==(request|state))return record(s,Action::Skip,R3::pwrWellDriver,ctl,ctl)?Result::OK:Result::LogFull;
  Result r=rmw(s,R3::pwrWellDriver,0,request);
  if(r==Result::OK)r=waitBits(s,R3::pwrWellDriver,state,state,1000);
  if(r==Result::OK)r=waitBits(s,R::fuseStatus,fuse,fuse,1000);
  return r;
 }
 // combo_pll_enable: power, CFGCR0/1, enable and lock.
 Result dpll0(const ReimsHdmiPll::Cfg&pll){
  const uint32_t en=io.read(R3::dpll0Enable);
  if(en&R3::pllEnable){
   const bool same=io.read(R3::dpll0Cfgcr0)==pll.cfgcr0&&io.read(R3::dpll0Cfgcr1)==pll.cfgcr1&&(en&R3::pllLock);
   if(!same)return fail(Step::DpllConfig,Result::Unexpected,R3::dpll0Enable,en);
   return record(Step::DpllEnable,Action::Skip,R3::dpll0Enable,en,en)?Result::OK:Result::LogFull;
  }
  Result r=rmw(Step::DpllPower,R3::dpll0Enable,0,R3::pllPowerEnable);
  if(r==Result::OK)r=waitBits(Step::DpllPower,R3::dpll0Enable,R3::pllPowerState,R3::pllPowerState,1000);
  if(r==Result::OK)r=write(Step::DpllConfig,R3::dpll0Cfgcr0,pll.cfgcr0,0,true);
  if(r==Result::OK)r=write(Step::DpllConfig,R3::dpll0Cfgcr1,pll.cfgcr1,1,true);
  if(r==Result::OK)r=rmw(Step::DpllEnable,R3::dpll0Enable,0,R3::pllEnable);
  if(r==Result::OK)r=waitBits(Step::DpllEnable,R3::dpll0Enable,R3::pllLock,R3::pllLock,1000);
  return r;
 }
 // icl_combo_phy_set_signal_levels with icl_ddi_combo_vswing_program.
 Result signalLevels(){
  Result r=write(Step::PhyKeeper,R3::pcsDw1Grp,io.read(R3::pcsDw1Ln0)&~R3::commonKeeper);
  for(unsigned ln=0;ln<4&&r==Result::OK;++ln) // 4 lanes at <= 6 GHz: LN0=0, LN1-3=1
   r=rmw(Step::PhyLoadgen,R3::txLane(ln,4),R3::loadgenSelect,ln>=1?R3::loadgenSelect:0,uint8_t(ln));
  if(r==Result::OK)r=rmw(Step::PhySusClock,R3::clDw5,0,R3::susClockConfig);
  if(r==Result::OK)r=write(Step::PhyTraining,R3::txDw5Grp,io.read(R3::txLane(0,5))&~R3::txTrainingEn);
  if(r==Result::OK){
   uint32_t v=io.read(R3::txLane(0,5));
   v&=~(R3::scalingModeMask|R3::rtermMask|R3::coeffPolarity|R3::cursorProgram|R3::tap2Disable|R3::tap3Disable);
   v|=2U<<18|6U<<3|R3::tap3Disable;
   r=write(Step::PhySwing,R3::txDw5Grp,v);
  }
  for(unsigned ln=0;ln<4&&r==Result::OK;++ln) // DW2: swing select, RCOMP scalar 0x98
   r=rmw(Step::PhySwing,R3::txLane(ln,2),1U<<15|7U<<11|0xffU,((kSwing>>3)&1)<<15|(kSwing&7)<<11|0x98,uint8_t(ln));
  for(unsigned ln=0;ln<4&&r==Result::OK;++ln) // DW4 per lane: GRP would overwrite loadgen
   r=rmw(Step::PhySwing,R3::txLane(ln,4),0x3fU<<12|0x3fU<<6|0x3fU,kPostCursor1<<12|kPostCursor2<<6|kCursorCoeff,uint8_t(4+ln));
  for(unsigned ln=0;ln<4&&r==Result::OK;++ln) // DW7: N scalar
   r=rmw(Step::PhySwing,R3::txLane(ln,7),0x7fU<<24,kNScalar<<24,uint8_t(8+ln));
  if(r==Result::OK)r=write(Step::PhyTraining,R3::txDw5Grp,io.read(R3::txLane(0,5))|R3::txTrainingEn,1);
  return r;
 }
public:
 using CoreInit<IO>::CoreInit;
 // Stage 2 (already applied or run here), then stage 3a for one mode.
 Result runOutput(const HdmiMode&m){
  Result r=CoreInit<IO>::run();
  if(r!=(execute?Result::OK:Result::Planned))return r;
  record(Step::Output,Action::Check,R3::transConfA,m.clockKHz,m.hActive<<16|m.vActive);
  // Above 340 MHz needs HDMI 2.0 scrambling at the 1/40 bit-clock ratio.
  if(m.clockKHz>340000&&!m.scramble)return fail(Step::Output,Result::PreconditionFailed,0,m.clockKHz);
  const auto pll=ReimsHdmiPll::compute(m.clockKHz);
  if(!pll.ok)return fail(Step::Output,Result::PreconditionFailed,0,m.clockKHz);
  // Never reprogram a running pipe or port.
  const uint32_t conf=io.read(R3::transConfA),buf=io.read(R3::ddiBufTc1);
  if((conf&R3::transEnable)||(buf&R3::ddiBufEnable))
   return fail(Step::Output,Result::PreconditionFailed,conf&R3::transEnable?R3::transConfA:R3::ddiBufTc1,conf&R3::transEnable?conf:buf);
  r=powerWell(Step::PowerWell2,R3::pw2Request,R3::pw2State,R3::fusePG2);
  if(r==Result::OK)r=powerWell(Step::PowerWell3,R3::pw3Request,R3::pw3State,R3::fusePG3);
  if(r==Result::OK)r=dpll0(pll);
  // adls_ddi_enable_clock: select DPLL0 for PHY B, then ungate separately.
  if(r==Result::OK)r=rmw(Step::DdiClock,R3::dpclka0,R3::phyBClkSelMask,0);
  if(r==Result::OK)r=rmw(Step::DdiClock,R3::dpclka0,R3::phyBClkOff,0,1);
  if(r==Result::OK)r=rmw(Step::DdiIoPower,R3::ddiPwrDriver,0,R3::tc1IoRequest);
  if(r==Result::OK)r=waitBits(Step::DdiIoPower,R3::ddiPwrDriver,R3::tc1IoState,R3::tc1IoState,1000);
  if(r==Result::OK)r=write(Step::TransClock,R3::transClkSelA,R3::transClkTc1,0,true);
  // AVI infoframe (hsw_set_infoframes/hsw_write_infoframe), set up while the
  // transcoder DDI function is still disabled: clear all DIP enables, write
  // the 8 AVI data dwords, then set the AVI enable. Full-range RGB so the
  // monitor does not treat our 0-255 output as limited range.
  if(r==Result::OK)r=write(Step::Infoframes,R3::dipCtlA,0,0,true);
  if(r==Result::OK){
   const auto avi=ReimsAVI::build(ReimsAVI::vicFor2160p(m.refreshHz),ReimsAVI::Range::Full);
   for(unsigned i=0;i<8&&r==Result::OK;++i)r=write(Step::Infoframes,R3::dipAviDataA+i*4,avi.dw[i],uint8_t(1+i));
  }
  if(r==Result::OK)r=write(Step::Infoframes,R3::dipCtlA,R3::dipEnableAvi,9,true);
  if(r==Result::OK)r=write(Step::PipeSrc,R3::pipeSrcA,(m.hActive-1)<<16|(m.vActive-1),0,true);
  if(r==Result::OK)r=write(Step::PipeMisc,R3::pipeMiscA,R3::pipeMiscValue,0,true);
  // intel_set_transcoder_timings (display 12: vblank start = vactive + SCL 0).
  const uint32_t t[][2]={{R3::vsyncShiftA,0},
   {R3::htotalA,(m.hTotal-1)<<16|(m.hActive-1)},{R3::hblankA,(m.hTotal-1)<<16|(m.hActive-1)},
   {R3::hsyncA,(m.hSyncEnd-1)<<16|(m.hSyncStart-1)},{R3::vtotalA,(m.vTotal-1)<<16|(m.vActive-1)},
   {R3::vblankA,(m.vTotal-1)<<16|(m.vActive-1)},{R3::vsyncA,(m.vSyncEnd-1)<<16|(m.vSyncStart-1)}};
  for(unsigned i=0;i<sizeof(t)/sizeof(t[0])&&r==Result::OK;++i)r=write(Step::Timings,t[i][0],t[i][1],uint8_t(i),true);
  if(r==Result::OK)r=write(Step::TransMult,R3::transMultA,0,0,true);
  if(r==Result::OK)r=rmw(Step::FrameStart,R3::chickenTransA,R3::frameStartDelayMask,0); // framestart_delay 1
  if(r==Result::OK)r=write(Step::TransConf,R3::transConfA,0,0,true); // progressive, not yet enabled
  // skl_linetime_wm: DIV_ROUND_UP(htotal * 8000, pixel rate in kHz).
  if(r==Result::OK)r=write(Step::Linetime,R3::linetimeA,(m.hTotal*8000+m.clockKHz-1)/m.clockKHz,0,true);
  if(r==Result::OK)r=rmw(Step::PipeChicken,R3::pipeChickenA,0,R3::pipeChickenBits);
  if(r==Result::OK)r=rmw(Step::MbusDbox,R3::mbusDboxA,R3::mbusDboxMask,R3::mbusDboxValue);
  // intel_ddi_enable: FUNC_CTL2, FUNC_CTL (HDMI, TC1, 8 bpc, sync polarity).
  // TRANS_DDI_HDMI_SCRAMBLING (bit 0) and TRANS_DDI_HIGH_TMDS_CHAR_RATE
  // (bit 4) for >340 MHz, matching the host's 4K60 func word 0xa0030011.
  const uint32_t func=1U<<31|4U<<27|(m.vPos?1U<<17:0)|(m.hPos?1U<<16:0)|
   (m.scramble?(1U<<0)|(1U<<4):0);
  if(r==Result::OK)r=write(Step::DdiFunc,R3::ddiFunc2A,0,0,true);
  if(r==Result::OK)r=write(Step::DdiFunc,R3::ddiFuncA,func,1,true);
  // intel_enable_transcoder: display 12 has a frame counter, so i915 does
  // not wait for the scanline here; the readback below checks the pipe runs.
  if(r==Result::OK)r=rmw(Step::TransEnable,R3::transConfA,0,R3::transEnable);
  // intel_ddi_enable_hdmi: signal levels, lanes, SCDC, buffer.
  if(r==Result::OK)r=signalLevels();
  if(r==Result::OK)r=rmw(Step::PhyLanes,R3::clDw10,R3::pwrDownLanesMask,0); // 4 lanes, no reversal
  // SCDC: set 1/40 bit-clock ratio and scrambling on the sink before the port
  // buffer enables (intel_hdmi_handle_sink_scrambling). The dry run records a
  // plan; the live I2C only runs in the execute build (it drives GMBUS).
  if(r==Result::OK&&m.scramble){
   if(!execute)record(Step::ScdcSetup,Action::Plan,R3::ddiBufTc1,0,1);
   else{
    ReimsSCDC::Scdc<IO> scdc(io);
    const auto ss=scdc.setup(2,true,true);           // DDC pin 2 (HDMI-B)
    const bool live=ss==ReimsSCDC::Status::OK&&scdc.scramblingStatus(2);
    record(Step::ScdcSetup,live?Action::Write:Action::Fail,R3::ddiBufTc1,uint32_t(ss),live);
    if(!live)r=Result::Unexpected;
   }
  }
  if(r==Result::OK)r=rmw(Step::DdiBuf,R3::ddiBufTc1,0,R3::ddiBufEnable);
  if(r==Result::OK)r=waitBits(Step::DdiBuf,R3::ddiBufTc1,R3::ddiBufIdle,0,10000);
  if(r!=Result::OK||!execute)return r==Result::OK?Result::Planned:r;
  // Readback: the scanline moves and the decoder reconstructs the mode.
  const uint32_t dsl0=io.read(R3::pipeDslA)&0xfffff;io.delayUS(1000);
  const uint32_t dsl1=io.read(R3::pipeDslA)&0xfffff;
  record(Step::Verify,dsl0!=dsl1?Action::Check:Action::Fail,R3::pipeDslA,dsl0,dsl1);
  if(dsl0==dsl1)return Result::Unexpected;
  ReimsDisplayTiming::Snapshot s;
  for(unsigned i=0;i<ReimsDisplayTiming::count;++i)s.value[i]=io.read(ReimsDisplayTiming::offsets[i]);
  ReimsDisplayTiming::Mode d;
  const auto st=ReimsDisplayTiming::decode(s,0,d);
  const bool match=st==ReimsDisplayTiming::Status::OK&&d.hActive==m.hActive&&d.vActive==m.vActive&&
   d.hTotal==m.hTotal&&d.vTotal==m.vTotal&&d.port==3&&d.phy==1&&d.pll==0&&!d.dp&&
   d.pixelClockHz+1000>=uint64_t(m.clockKHz)*1000&&d.pixelClockHz<=uint64_t(m.clockKHz)*1000+1000;
  record(Step::Verify,match?Action::Check:Action::Fail,R3::transConfA,uint32_t(st),uint32_t(d.pixelClockHz/1000));
  if(!match)return Result::Unexpected;
  record(Step::Done,Action::Check,0,0,0);
  return Result::OK;
 }
};
}
