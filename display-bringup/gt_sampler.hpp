#pragma once
#include <stdint.h>
#include <stddef.h>

// Read-only GT watch for the phase-6 accelerator start. The first start on
// a780 (2026-10-13) wedged IOKit: ioreg and logd blocked, so neither registry
// properties nor IOLog could report where the GPU init stopped. A timer in
// the bring-up kext samples these registers and prints them with kprintf,
// which goes to the serial console without IOKit or logd.
//
// Reads only. It never requests forcewake, so GT-domain registers read as 0
// while the GT sleeps; the forcewake acks in the same sample show which. The
// GGTT entries live in BAR0's upper half and need no forcewake.
// Offsets: i915 intel_gt_regs.h, intel_engine_regs.h (RENDER 0x2000,
// BLT 0x22000), gt/uc/intel_guc_reg.h; display ones as in hdmi_scanout.hpp.
namespace ReimsGTSampler {
struct Reg {uint32_t offset;const char*name;};
constexpr Reg regs[]={
 {0x130044,"fwAckGT"},{0x000d84,"fwAckRender"},
 {0x00c000,"gucStatus"},{0x00c180,"gucScratch0"},{0x00c314,"gucDmaCtrl"},
 {0x002034,"rcsHead"},{0x002030,"rcsTail"},{0x002038,"rcsStart"},{0x00203c,"rcsCtl"},
 {0x002074,"rcsActhd"},{0x002068,"rcsIpehr"},{0x002234,"rcsExecStatus"},{0x0023a0,"rcsCsb"},
 {0x0020d0,"rcsReset"},{0x00209c,"rcsMiMode"},
 {0x022034,"bcsHead"},{0x022030,"bcsTail"},{0x02203c,"bcsCtl"},{0x022074,"bcsActhd"},
 {0x0040a0,"errorGen6"},{0x00cec4,"ringFault"},
 {0x070008,"transconfA"},{0x0701ac,"surfLiveA"},
 // GGTT PTE of the bring-up's first framebuffer page (GGTT 0x01000000):
 // ggttBase 0x800000 + (0x01000000 >> 12) * 8, as hdmi_scanout.hpp writes it.
 // A change during PrepareRuntime means the accelerator reused the range.
 {0x808000,"fbPteLo"},{0x808004,"fbPteHi"},
};
constexpr unsigned count=sizeof(regs)/sizeof(regs[0]);
constexpr uint32_t maxOffset(){uint32_t m=0;for(const auto&r:regs)if(r.offset>m)m=r.offset;return m;}

struct Sample {uint32_t v[count]={};};

// Print policy: every sample that differs from the last printed one, plus a
// heartbeat (an unchanged GT then reads as "frozen", not as "sampler dead"),
// up to a line budget so a busy start cannot flood the console.
struct Policy {
 uint32_t heartbeatTicks=20;  // 5 s at 250 ms
 uint32_t maxLines=4000;
};
struct State {
 Sample last;bool havePrinted=false;uint32_t sinceHeartbeat=0,lines=0;
 bool shouldPrint(const Sample&s,const Policy&p){
  if(lines>=p.maxLines)return false;
  bool changed=!havePrinted;
  for(unsigned i=0;i<count&&!changed;++i)changed=s.v[i]!=last.v[i];
  if(!changed&&++sinceHeartbeat<p.heartbeatTicks)return false;
  last=s;havePrinted=true;sinceHeartbeat=0;++lines;return true;
 }
};

// "ReimsGT <seq> <ms> v0 v1 ...": fixed register order, 8 hex digits each.
// The register names are printed once with header(). Returns the length.
inline size_t format(char*out,size_t n,uint32_t seq,uint64_t ms,const Sample&s){
 static const char hex[]="0123456789abcdef";
 size_t len=0;
 auto put=[&](char c){if(len+1<n)out[len]=c;++len;};
 auto dec=[&](uint64_t v){char d[20];unsigned k=0;do{d[k++]=char('0'+v%10);v/=10;}while(v);while(k)put(d[--k]);};
 for(const char*p="ReimsGT ";*p;++p)put(*p);
 dec(seq);put(' ');dec(ms);
 for(unsigned i=0;i<count;++i){put(' ');for(int b=28;b>=0;b-=4)put(hex[(s.v[i]>>b)&0xf]);}
 put('\n');
 if(n)out[len<n?len:n-1]='\0';
 return len;
}
inline size_t header(char*out,size_t n){
 size_t len=0;
 auto put=[&](char c){if(len+1<n)out[len]=c;++len;};
 for(const char*p="ReimsGT seq ms";*p;++p)put(*p);
 for(unsigned i=0;i<count;++i){put(' ');for(const char*p=regs[i].name;*p;++p)put(*p);}
 put('\n');
 if(n)out[len<n?len:n-1]='\0';
 return len;
}
}
