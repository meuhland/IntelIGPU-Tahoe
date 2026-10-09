// Host-side test of the RPL-S display_timing.hpp decoder with synthetic
// register snapshots programmed the way Linux i915 would. Build with
// -DREIMS_TARGET_RPLS (scripts/test.sh does).
#include <stdio.h>
#include <stdlib.h>
#include "display_timing.hpp"

#if !defined(REIMS_TARGET_RPLS)
#error "build with -DREIMS_TARGET_RPLS"
#endif

using namespace ReimsDisplayTiming;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

static void set(Snapshot&s,uint32_t a,uint32_t v){
 for(unsigned i=0;i<count;++i)if(offsets[i]==a){s.value[i]=v;return;}
 printf("FAIL register 0x%x not captured\n",a);++failures;
}
struct Timing {uint32_t ha,hs,he,ht,va,vs,ve,vt;};
// i915 programs (value-1) pairs: total|active, end|start.
static void timing(Snapshot&s,unsigned t,const Timing&g){
 const uint32_t T=t*0x1000;
 set(s,0x60000+T,(g.ht-1)<<16|(g.ha-1));set(s,0x60008+T,(g.he-1)<<16|(g.hs-1));
 set(s,0x6000c+T,(g.vt-1)<<16|(g.va-1));set(s,0x60014+T,(g.ve-1)<<16|(g.vs-1));
}
// Combo PLL as icl_calc_dpll_state writes it, including WA #22010492432.
static void pll(Snapshot&s,unsigned id,unsigned refCode,uint64_t portHz,unsigned p,unsigned q,unsigned k){
 const double ref=refCode?19.2e6:24e6,dco=double(portHz)*5*p*q*k/ref;
 uint32_t integer=uint32_t(dco),fraction=uint32_t((dco-integer)*32768+0.5);
 if(refCode==2)fraction=(fraction+1)/2;
 const uint32_t pc=p==2?1:p==3?2:p==5?4:8,kc=k==1?1:k==2?2:4;
 set(s,0x51004,refCode<<29);
 set(s,pllEnable[id],0xc0000000U);
 set(s,pllCfgcr0[id],fraction<<10|integer);
 set(s,pllCfgcr0[id]+4,(q>1?(q<<10|1U<<9):0)|kc<<6|pc<<2);
}
// Enable transcoder t on DDI port, clocked from DPLL id. mode: 0 HDMI, 2 DP SST.
static void route(Snapshot&s,unsigned t,unsigned port,unsigned id,unsigned mode,unsigned bpcCode,unsigned lanes){
 const uint32_t T=t*0x1000,phy=port?port-2:0;
 set(s,0x60400+T,0x80000000U|(port+1)<<27|mode<<24|bpcCode<<20|1U<<16|(lanes?lanes-1:0)<<1);
 set(s,0x70008+T,0xc0000000U);
 set(s,0x46140+t*4,(port+1)<<28);
 const uint32_t reg=phy<3?0x164280:0x1642bc;
 Snapshot copy=s;uint32_t clka=copy.get(reg);
 clka=(clka&~(3U<<((phy%3)*2)))|id<<((phy%3)*2);
 set(s,reg,clka);
}
static bool near(uint64_t a,uint64_t b,uint64_t tol){return a>b?a-b<=tol:b-a<=tol;}

int main(){
 // 1920x1080@60 HDMI 8bpc, transcoder A -> DDI A (PHY A), DPLL0, 38.4MHz ref.
 {Snapshot s;timing(s,0,{1920,2008,2052,2200,1080,1084,1089,1125});
  pll(s,0,2,148500000,3,2,2);route(s,0,0,0,0,0,0);
  Mode m;CHECK(decode(s,0,m)==Status::OK);
  CHECK(!m.dp&&m.port==0&&m.phy==0&&m.pll==0&&m.bpc==8);
  CHECK(near(m.portClockHz,148500000,1000)&&near(m.pixelClockHz,148500000,1000));
  CHECK(m.hActive==1920&&m.hTotal==2200&&m.hSyncStart==2008&&m.hSyncEnd==2052);
  CHECK(m.vActive==1080&&m.vTotal==1125&&m.vSyncStart==1084&&m.vSyncEnd==1089);
  CHECK(near(m.refresh1616,60u<<16,70)&&m.hPositive&&!m.vPositive);
  // Clock gated off at DPCLKA (PHY A bit 10).
  Snapshot off=s;set(off,0x164280,off.get(0x164280)|1U<<10);CHECK(decode(off,0,m)==Status::Disabled);
  // TRANS_CLK_SEL pointing at another port.
  Snapshot clk=s;set(clk,0x46140,4U<<28);CHECK(decode(clk,0,m)==Status::Route);
  // PLL enabled but not locked.
  Snapshot lock=s;set(lock,0x46010,0x80000000U);CHECK(decode(lock,0,m)==Status::PLL);
  // VRR live.
  Snapshot vrr=s;set(vrr,0x6042c,1U<<27);CHECK(decode(vrr,0,m)==Status::VariableRefresh);
  // Interlaced.
  Snapshot il=s;set(il,0x70008,0xc0000000U|3U<<21);CHECK(decode(il,0,m)==Status::Geometry);
  // YCbCr 4:2:0 doubles the HDMI dot clock; TRANS_MULT divides it.
  Snapshot y=s;set(y,0x70030,1U<<27);CHECK(decode(y,0,m)==Status::OK&&near(m.pixelClockHz,297000000,2000));
  Snapshot mul=s;set(mul,0x6002c,1);CHECK(decode(mul,0,m)==Status::OK&&near(m.pixelClockHz,74250000,1000));
  // DDI B does not exist on ADL-S/RPL-S; DP MST is not decoded.
  Snapshot b=s;set(b,0x60400,(s.get(0x60400)&~(15U<<27))|2U<<27);set(b,0x46140,2U<<28);
  CHECK(decode(b,0,m)==Status::Route);
  Snapshot mst=s;set(mst,0x60400,s.get(0x60400)|3U<<24);CHECK(decode(mst,0,m)==Status::Route);
 }
 // 2560x1440@60 CVT-RB DP SST 4 lanes HBR2, transcoder B -> TC2 (port 4,
 // PHY C, DPCLKA0 bits 5:4), DPLL2 (DPLL4 CFGCR registers), 38.4MHz ref.
 {Snapshot s;timing(s,1,{2560,2608,2640,2720,1440,1443,1448,1481});
  pll(s,2,2,540000000,3,1,1);route(s,1,4,2,2,0,4);
  set(s,0x61040,241500);set(s,0x61044,540000);
  Mode m;CHECK(decode(s,1,m)==Status::OK);
  CHECK(m.dp&&m.port==4&&m.phy==2&&m.pll==2&&m.lanes==4);
  CHECK(near(m.portClockHz,540000000,1000)&&near(m.pixelClockHz,241500000,1000));
  CHECK(near(m.refresh1616,uint32_t(59.95*65536),300));
  // Single-output scan finds transcoder B; A is disabled.
  Mode first;CHECK(decode(s,first)==Status::OK&&first.transcoder==1);
  // Clock gated off at PHY C bit 24.
  Snapshot off=s;set(off,0x164280,s.get(0x164280)|1U<<24);CHECK(decode(off,1,m)==Status::Disabled);
  // 1 lane HBR2 (4.32Gb/s) cannot carry 241.5MHz at 10bpc (7.25Gb/s).
  Snapshot bw=s;set(bw,0x61400,(s.get(0x61400)&~(7U<<1|7U<<20))|1U<<20);
  CHECK(decode(bw,1,m)==Status::Link);
 }
 // 3840x2160@30 HDMI 10bpc, transcoder D -> TC3 (port 5, PHY D, DPCLKA1
 // bits 1:0, clock-off bit 4), DPLL3, 24MHz ref.
 {Snapshot s;timing(s,3,{3840,4016,4104,4400,2160,2168,2178,2250});
  pll(s,3,0,371250000,5,1,1);route(s,3,5,3,0,1,0);
  Mode m;CHECK(decode(s,3,m)==Status::OK);
  CHECK(!m.dp&&m.port==5&&m.phy==3&&m.pll==3&&m.bpc==10);
  CHECK(near(m.portClockHz,371250000,1000)&&near(m.pixelClockHz,297000000,1000));
  CHECK(near(m.refresh1616,30u<<16,70));
  Snapshot off=s;set(off,0x1642bc,s.get(0x1642bc)|1U<<4);CHECK(decode(off,3,m)==Status::Disabled);
 }
 // Real RPL-S capture (host i915, 2026-10-09): 3840x2160@60 HDMI 2.0 on
 // TC1/PHY B via DPLL0, scrambling bits set. i915 reported the same mode.
 {static const uint32_t real[][2]={
   {0x46010,0xcc000000},{0x46140,0x40000000},{0x51004,0x40000020},
   {0x60000,0x112f0eff},{0x60008,0x10070faf},{0x6000c,0x08c9086f},{0x60014,0x08810877},
   {0x60400,0xa0030011},{0x60420,0x600002b9},{0x61400,0x00030000},{0x61420,0x00000100},
   {0x70008,0xc0000000},{0x70030,0x00800100},{0x71008,0x00000024},{0x71030,0x01000000},
   {0x164280,0x01e07400},{0x164284,0x001001d0},{0x164288,0x00000448},
   {0x16428c,0x018001d4},{0x164290,0x00002644},{0x164294,0x018001d4},{0x164298,0x00002644},
   {0x1642bc,0x00000030},{0x1642c0,0x018001d4},{0x1642c4,0x00002644}};
  Snapshot s;for(const auto&r:real)set(s,r[0],r[1]);
  Mode m;CHECK(decode(s,0,m)==Status::OK);
  CHECK(!m.dp&&m.port==3&&m.phy==1&&m.pll==0&&m.bpc==8&&m.hPositive&&m.vPositive);
  CHECK(m.portClockHz==594000000&&m.pixelClockHz==594000000&&m.refresh1616==60u<<16);
  CHECK(m.hActive==3840&&m.hTotal==4400&&m.hSyncStart==4016&&m.hSyncEnd==4104);
  CHECK(m.vActive==2160&&m.vTotal==2250&&m.vSyncStart==2168&&m.vSyncEnd==2178);
  for(unsigned t=1;t<transcoders;++t)CHECK(decode(s,t,m)==Status::Disabled);
 }
 // Nothing enabled.
 {Snapshot s;Mode m;CHECK(decode(s,m)==Status::Disabled);CHECK(decode(s,4,m)==Status::Route);}
 if(failures){printf("%d failure(s)\n",failures);return 1;}
 printf("PASS: RPL-S display timing decode (HDMI/DP, DPLL0-3, PHY A-E routing)\n");
 return 0;
}
