#pragma once
#include <stdint.h>

// Combo PHY DPLL settings for an HDMI port clock, ported from Linux i915 v7.2
// intel_dpll_mgr.c: icl_calc_wrpll, icl_wrpll_get_multipliers,
// icl_wrpll_params_populate and icl_calc_dpll_state (display 12, 38.4 MHz
// reference: the DPLL halves it to 19.2 MHz, and WA #22010492432 stores half
// the DCO fraction). Frequencies in kHz, as in i915.
namespace ReimsHdmiPll {
struct Cfg {uint32_t cfgcr0=0,cfgcr1=0,dcoKHz=0;bool ok=false;};
inline Cfg compute(uint32_t portClockKHz){
 Cfg c;
 static const int dividers[]={2,4,6,8,10,12,14,16,18,20,24,28,30,32,36,40,
  42,44,48,50,52,54,56,60,64,66,68,70,72,76,78,80,84,88,90,92,96,98,100,102,
  3,5,7,9,15,21};
 const uint32_t refKHz=19200,afe=portClockKHz*5,dcoMin=7998000,dcoMax=10000000,
  dcoMid=(dcoMin+dcoMax)/2;
 uint32_t bestDco=0,bestCentrality=~0U;int bestDiv=0;
 for(int d:dividers){
  const uint64_t dco=uint64_t(afe)*d;
  if(dco<dcoMin||dco>dcoMax)continue;
  const uint32_t centrality=dco>dcoMid?uint32_t(dco-dcoMid):uint32_t(dcoMid-dco);
  if(centrality<bestCentrality){bestCentrality=centrality;bestDiv=d;bestDco=uint32_t(dco);}
 }
 if(!bestDiv)return c;
 int p=0,q=0,k=0;
 if(bestDiv%2==0){
  if(bestDiv==2){p=2;q=1;k=1;}
  else if(bestDiv%4==0){p=2;q=bestDiv/4;k=2;}
  else if(bestDiv%6==0){p=3;q=bestDiv/6;k=2;}
  else if(bestDiv%5==0){p=5;q=bestDiv/10;k=2;}
  else if(bestDiv%14==0){p=7;q=bestDiv/14;k=2;}
 }else if(bestDiv==3||bestDiv==5||bestDiv==7){p=bestDiv;q=1;k=1;}
 else{p=bestDiv/3;q=1;k=3;}
 const uint32_t kCode=k==1?1:k==2?2:k==3?4:0,pCode=p==2?1:p==3?2:p==5?4:p==7?8:0;
 if(!kCode||!pCode||!q)return c;
 const uint64_t dco=(uint64_t(bestDco)<<15)/refKHz;
 const uint32_t integer=uint32_t(dco>>15),fraction=uint32_t(dco&0x7fff);
 const uint32_t storedFraction=(fraction+1)/2; // DIV_ROUND_CLOSEST(fraction, 2)
 c.cfgcr0=storedFraction<<10|integer;
 // QDIV_RATIO 17:10, QDIV_MODE 9, KDIV 8:6, PDIV 5:2; CFSELOVRD normal xtal 0.
 c.cfgcr1=uint32_t(q)<<10|(q==1?0U:1U)<<9|kCode<<6|pCode<<2;
 c.dcoKHz=bestDco;c.ok=true;
 return c;
}
}
