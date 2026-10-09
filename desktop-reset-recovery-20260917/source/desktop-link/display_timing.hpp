#pragma once
#include <stdint.h>

#if defined(REIMS_TARGET_RPLS)
// RPL-S (ADL-S display, version 12): transcoders A-D; DDI A and TC1-TC4
// (ports 0,3-6) on combo PHYs A-E; DPLL0-3; DP SST or HDMI/DVI. Read-only
// reconstruction of the running mode, not a table of advertised frequencies.
// Linux i915 v7.2: intel_display_regs.h, intel_vrr_regs.h; intel_dpll_mgr.c
// (adls_plls, ADLS_DPLL_CFGCR*, icl_ddi_combo_pll_get_freq); intel_ddi.c
// (adls_ddi_get_pll); intel_display.c (intel_port_to_phy, intel_crtc_dotclock).
namespace ReimsDisplayTiming {
// Per transcoder t (pipe t): timings, TRANS_MULT, link M/N, DDI_FUNC_CTL,
// VRR_CTL/STATUS, TRANSCONF, PIPE_MISC, TRANS_CLK_SEL.
#define REIMS_TRANSCODER_REGS(t) \
 0x60000+(t)*0x1000,0x60008+(t)*0x1000,0x6000c+(t)*0x1000,0x60014+(t)*0x1000, \
 0x6002c+(t)*0x1000,0x60040+(t)*0x1000,0x60044+(t)*0x1000,0x60400+(t)*0x1000, \
 0x60420+(t)*0x1000,0x6042c+(t)*0x1000,0x70008+(t)*0x1000,0x70030+(t)*0x1000, \
 0x46140+(t)*4
static constexpr uint32_t offsets[]={
 REIMS_TRANSCODER_REGS(0),REIMS_TRANSCODER_REGS(1),
 REIMS_TRANSCODER_REGS(2),REIMS_TRANSCODER_REGS(3),
 0x51004,0x164280,0x1642bc,0x46010,0x46014,0x46018,0x46030,
 0x164284,0x164288,0x16428c,0x164290,0x164294,0x164298,0x1642c0,0x1642c4};
#undef REIMS_TRANSCODER_REGS
static constexpr unsigned count=sizeof(offsets)/sizeof(offsets[0]);
static constexpr unsigned transcoders=4;
// DPLL0-3 enable and CFGCR0 (CFGCR1 is +4); DPLL2 uses the DPLL4 registers.
static constexpr uint32_t pllEnable[]={0x46010,0x46014,0x46018,0x46030};
static constexpr uint32_t pllCfgcr0[]={0x164284,0x16428c,0x164294,0x1642c0};
// DPCLKA clock-off bit per PHY A-E (ICL_DPCLKA_CFGCR0_DDI_CLK_OFF).
static constexpr unsigned clockOff[]={10,11,24,4,5};
struct Snapshot {
 uint32_t value[count]={};
 uint32_t get(uint32_t address)const{
  for(unsigned i=0;i<count;++i)if(offsets[i]==address)return value[i];
  return 0;
 }
};
struct Mode {
 uint32_t hActive=0,hTotal=0,hSyncStart=0,hSyncEnd=0;
 uint32_t vActive=0,vTotal=0,vSyncStart=0,vSyncEnd=0;
 uint32_t refresh1616=0,pll=0,lanes=0,bpc=0;
 uint32_t transcoder=0,port=0,phy=0;
 uint64_t portClockHz=0,pixelClockHz=0;
 bool hPositive=false,vPositive=false,dp=false;
};
enum class Status:uint32_t {OK,Route,Disabled,VariableRefresh,PLL,Geometry,Clock,Link};
inline Status decode(const Snapshot&s,unsigned t,Mode&m){
 m={};m.transcoder=t;if(t>=transcoders)return Status::Route;
 const uint32_t T=t*0x1000,ddi=s.get(0x60400+T),conf=s.get(0x70008+T);
 if(!(ddi&0x80000000U)||!(conf&0x40000000U))return Status::Disabled;
 const uint32_t select=(ddi>>27)&15;
 if(!select)return Status::Route;
 m.port=select-1;
 if(m.port==0)m.phy=0;else if(m.port>=3&&m.port<=6)m.phy=m.port-2;else return Status::Route;
 if(((s.get(0x46140+t*4)>>28)&15)!=select)return Status::Route;
 const uint32_t mode=(ddi>>24)&7;
 if(mode>2)return Status::Route; // DP MST and 128b/132b are not decoded.
 m.dp=mode==2;
 // Reject VRR and interlace instead of presenting an invented fixed mode.
 if((s.get(0x60420+T)&0x80000000U)||(s.get(0x6042c+T)&(1U<<27)))return Status::VariableRefresh;
 if(conf&(7U<<21))return Status::Geometry;
 const uint32_t dpclka=s.get(m.phy<3?0x164280:0x1642bc);
 if(dpclka&(1U<<clockOff[m.phy]))return Status::Disabled;
 m.pll=(dpclka>>((m.phy%3)*2))&3;
 if((s.get(pllEnable[m.pll])&0xc0000000U)!=0xc0000000U)return Status::PLL;
 const uint32_t refCode=s.get(0x51004)>>29;
 if(refCode>2)return Status::Clock;
 // The DPLL halves a 38.4MHz reference internally.
 const uint64_t ref=refCode?19200000:24000000;
 const uint32_t c0=s.get(pllCfgcr0[m.pll]),c1=s.get(pllCfgcr0[m.pll]+4);
 const unsigned pCode=(c1>>2)&15,kCode=(c1>>6)&7;
 const unsigned p=pCode==1?2:pCode==2?3:pCode==4?5:pCode==8?7:0;
 const unsigned k=kCode==1?1:kCode==2?2:kCode==4?3:0;
 const unsigned q=(c1&(1U<<9))?((c1>>10)&255):1;
 if(!p||!k||!q)return Status::PLL;
 // Display WA #22010492432 (display 12+): at 38.4MHz the fraction is halved.
 const uint64_t fraction=((c0>>10)&0x7fff)*(refCode==2?2U:1U);
 const uint64_t dco32768=(uint64_t(c0&1023)*32768+fraction)*ref;
 m.portClockHz=(dco32768+uint64_t(p)*q*k*5*16384)/(uint64_t(p)*q*k*5*32768);
 if(!m.portClockHz||m.portClockHz>810000000)return Status::Clock;
 const unsigned bpcCode=(ddi>>20)&7;
 if(bpcCode>3)return Status::Link;
 m.bpc=bpcCode==0?8:bpcCode==1?10:bpcCode==2?6:12;
 if(m.dp){
  const uint32_t lm=s.get(0x60040+T)&0xffffff,ln=s.get(0x60044+T)&0xffffff;
  if(!lm||!ln)return Status::Clock;
  m.pixelClockHz=(m.portClockHz*lm+ln/2)/ln;
 }else{
  // HDMI/DVI: the PLL runs at the TMDS character rate.
  if(m.bpc<8)return Status::Link;
  m.pixelClockHz=(m.portClockHz*8+m.bpc/2)/m.bpc;
  if(s.get(0x70030+T)&(1U<<27))m.pixelClockHz*=2; // YCbCr 4:2:0
 }
 const uint64_t multiplier=uint64_t(s.get(0x6002c+T))+1;
 if(multiplier>8)return Status::Clock;
 m.pixelClockHz=(m.pixelClockHz+multiplier/2)/multiplier;
 const uint32_t h=s.get(0x60000+T),v=s.get(0x6000c+T),hs=s.get(0x60008+T),vs=s.get(0x60014+T);
 m.hActive=(h&65535)+1;m.hTotal=(h>>16)+1;
 m.vActive=(v&65535)+1;m.vTotal=(v>>16)+1;
 m.hSyncStart=(hs&65535)+1;m.hSyncEnd=(hs>>16)+1;
 m.vSyncStart=(vs&65535)+1;m.vSyncEnd=(vs>>16)+1;
 if(m.hTotal<=m.hActive||m.vTotal<=m.vActive||m.hSyncStart<m.hActive||
    m.hSyncEnd<=m.hSyncStart||m.hSyncEnd>m.hTotal||m.vSyncStart<m.vActive||
    m.vSyncEnd<=m.vSyncStart||m.vSyncEnd>m.vTotal)return Status::Geometry;
 const uint64_t total=uint64_t(m.hTotal)*m.vTotal;
 const uint64_t refresh=(m.pixelClockHz*65536+total/2)/total;
 if(!refresh||refresh>0xffffffffULL)return Status::Clock;
 m.refresh1616=uint32_t(refresh);
 if(m.dp){
  m.lanes=((ddi>>1)&7)+1;
  if(m.lanes!=1&&m.lanes!=2&&m.lanes!=4)return Status::Link;
  if(m.pixelClockHz*3*m.bpc>m.portClockHz*8*m.lanes)return Status::Link;
 }
 m.hPositive=ddi&(1U<<16);m.vPositive=ddi&(1U<<17);
 return Status::OK;
}
// First enabled transcoder, for single-output callers.
inline Status decode(const Snapshot&s,Mode&m){
 for(unsigned t=0;t<transcoders;++t){
  const Status status=decode(s,t,m);
  if(status!=Status::Disabled)return status;
 }
 m={};return Status::Disabled;
}
}
#else
// ADL-P Display 13, transcoder A -> combo PHY A, DP SST. Read-only
// reconstruction of the running mode, not a table of advertised frequencies.
// Register/PLL formulas: Linux i915 intel_display_regs.h/intel_dpll_mgr.c.
namespace ReimsDisplayTiming {
static constexpr uint32_t offsets[]={
 0x60000,0x60004,0x60008,0x6000c,0x60010,0x60014,0x6001c,0x6007c,
 0x60030,0x60034,0x60040,0x60044,0x60400,0x46140,0x164280,0x51004,
 0x46010,0x46014,0x164284,0x164288,0x16428c,0x164290,
 0x60420,0x6042c,0x60424,0x60434,0x70008,0x70040,0x70000,0x70048,0x44070};
static constexpr unsigned count=sizeof(offsets)/sizeof(offsets[0]);
struct Snapshot {
 uint32_t value[count]={};
 uint32_t get(uint32_t address)const{
  for(unsigned i=0;i<count;++i)if(offsets[i]==address)return value[i];
  return 0;
 }
};
struct Mode {
 uint32_t hActive=0,hTotal=0,hSyncStart=0,hSyncEnd=0;
 uint32_t vActive=0,vTotal=0,vSyncStart=0,vSyncEnd=0;
 uint32_t refresh1616=0,pll=0,lanes=0,bpc=0;
 uint64_t portClockHz=0,pixelClockHz=0;
 bool hPositive=false,vPositive=false;
};
enum class Status:uint32_t {OK,Route,Disabled,VariableRefresh,PLL,Geometry,Clock,Link};
inline Status decode(const Snapshot&s,Mode&m){
 m={};const uint32_t ddi=s.get(0x60400),mux=s.get(0x164280);
 if(((ddi>>27)&15)!=1||((ddi>>24)&7)!=2||
    (s.get(0x46140)&0xf0000000U)!=0x10000000U)return Status::Route;
 if(!(ddi&0x80000000U)||!(s.get(0x70008)&0x40000000U)||(mux&(1U<<10)))return Status::Disabled;
 // Reject VRR and interlace instead of presenting an invented fixed mode.
 if((s.get(0x60420)&0x80000000U)||(s.get(0x6042c)&(1U<<27)))return Status::VariableRefresh;
 if(s.get(0x70008)&(7U<<21))return Status::Geometry;
 m.pll=mux&3;if(m.pll>1)return Status::PLL;
 const uint32_t enable=s.get(m.pll?0x46014:0x46010);
 if((enable&0xc0000000U)!=0xc0000000U)return Status::PLL;
 const uint32_t refCode=s.get(0x51004)>>29;
 if(refCode>2)return Status::Clock;
 const uint64_t ref=refCode?19200000:24000000;
 const uint32_t c0=s.get(m.pll?0x16428c:0x164284),c1=s.get(m.pll?0x164290:0x164288);
 const unsigned pCode=(c1>>2)&15,kCode=(c1>>6)&7;
 const unsigned p=pCode==1?2:pCode==2?3:pCode==4?5:pCode==8?7:0;
 const unsigned k=kCode==1?1:kCode==2?2:kCode==4?3:0;
 const unsigned q=(c1&(1U<<9))?((c1>>10)&255):1;
 if(!p||!k||!q)return Status::PLL;
 // Display WA #22010492432: at 38.4MHz ADLP stores half the fraction.
 const uint64_t fraction=((c0>>10)&0x7fff)*(refCode==2?2U:1U);
 const uint64_t dco32768=(uint64_t(c0&1023)*32768+fraction)*ref;
 m.portClockHz=(dco32768+uint64_t(p)*q*k*5*16384)/(uint64_t(p)*q*k*5*32768);
 const uint32_t lm=s.get(0x60040)&0xffffff,ln=s.get(0x60044)&0xffffff;
 if(!lm||!ln||!m.portClockHz||m.portClockHz>810000000)return Status::Clock;
 m.pixelClockHz=(m.portClockHz*lm+ln/2)/ln;
 const uint32_t h=s.get(0x60000),v=s.get(0x6000c),hs=s.get(0x60008),vs=s.get(0x60014);
 m.hActive=(h&65535)+1;m.hTotal=(h>>16)+1;
 m.vActive=(v&65535)+1;m.vTotal=(v>>16)+1;
 m.hSyncStart=(hs&65535)+1;m.hSyncEnd=(hs>>16)+1;
 m.vSyncStart=(vs&65535)+1;m.vSyncEnd=(vs>>16)+1;
 if(m.hTotal<=m.hActive||m.vTotal<=m.vActive||m.hSyncStart<m.hActive||
    m.hSyncEnd<=m.hSyncStart||m.hSyncEnd>m.hTotal||m.vSyncStart<m.vActive||
    m.vSyncEnd<=m.vSyncStart||m.vSyncEnd>m.vTotal)return Status::Geometry;
 const uint64_t total=uint64_t(m.hTotal)*m.vTotal;
 const uint64_t refresh=(m.pixelClockHz*65536+total/2)/total;
 if(!refresh||refresh>0xffffffffULL)return Status::Clock;
 m.refresh1616=uint32_t(refresh);
 const unsigned bpcCode=(ddi>>20)&7;
 if(bpcCode>3)return Status::Link;
 m.bpc=bpcCode==0?8:bpcCode==1?10:bpcCode==2?6:12;
 m.lanes=((ddi>>1)&7)+1;
 if(m.lanes!=1&&m.lanes!=2&&m.lanes!=4)return Status::Link;
 if(m.pixelClockHz*3*m.bpc>m.portClockHz*8*m.lanes)return Status::Link;
 m.hPositive=ddi&(1U<<16);m.vPositive=ddi&(1U<<17);
 return Status::OK;
}
}
#endif
