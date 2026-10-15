#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../display-bringup/gt_sampler.hpp"

// GT watch for the hold build (2026-10-15). The accelerator's start hangs
// silently after "Scheduler Throttle Cap" with the GT awake and the real
// topology, and no register view of that hang exists: the bring-up sampler
// never armed on 10-14 and stage 3 was skipped on 10-15. Two kernel threads:
// a sampler reads the ReimsGTSampler registers every 250 ms and publishes
// each complete sample; a reporter, which never touches MMIO, prints new
// samples with ReimsGTSampler's policy and, if the sampler stops advancing,
// a stall line naming the register it is stuck reading (an MMIO read that
// never completes) or saying it is not being scheduled.
namespace ReimsGTWatch {

// Sampler -> reporter handoff. The sampler fills buf[(seq+1)&1] and then
// publishes seq+1; the reporter copies buf[seq&1], which the sampler does not
// touch again until two samples later (500 ms), longer than one copy.
struct Shared {
 ReimsGTSampler::Sample buf[2];
 uint32_t seq=0;        // complete samples published
 int32_t reading=-1;    // register index being read, -1 between samples
};

enum Out:unsigned {none=0,sample=1,stall=2};

struct Reporter {
 ReimsGTSampler::State state;
 ReimsGTSampler::Policy policy;
 uint32_t lastSeq=0;
 uint64_t seqChangedMS=0,lastStallMS=0;
 bool stalled=false,started=false;
 static constexpr uint64_t stallMS=2000,stallRepeatMS=5000;
 // Called every 250 ms with what the sampler last published.
 unsigned tick(uint64_t nowMS,uint32_t seq,int32_t reading,const ReimsGTSampler::Sample&s){
  if(!started){started=true;lastSeq=seq;seqChangedMS=nowMS;}
  if(seq!=lastSeq){
   lastSeq=seq;seqChangedMS=nowMS;stalled=false;
   return state.shouldPrint(s,policy)?sample:none;
  }
  if(nowMS-seqChangedMS<stallMS)return none;
  if(stalled&&nowMS-lastStallMS<stallRepeatMS)return none;
  (void)reading;
  stalled=true;lastStallMS=nowMS;
  return stall;
 }
 uint64_t stuckMS(uint64_t nowMS)const{return nowMS-seqChangedMS;}
};

// "ReimsGT stall seq <n> ms <t> reading <name> 0x<offset> for <k> ms", or
// "... starved for <k> ms" when the sampler is between reads (not scheduled).
inline size_t formatStall(char*out,size_t n,uint32_t seq,uint64_t ms,int32_t reading,uint64_t stuck){
 static const char hex[]="0123456789abcdef";
 size_t len=0;
 auto put=[&](char c){if(len+1<n)out[len]=c;++len;};
 auto str=[&](const char*p){while(*p)put(*p++);};
 auto dec=[&](uint64_t v){char d[20];unsigned k=0;do{d[k++]=char('0'+v%10);v/=10;}while(v);while(k)put(d[--k]);};
 str("ReimsGT stall seq ");dec(seq);str(" ms ");dec(ms);
 if(reading>=0&&unsigned(reading)<ReimsGTSampler::count){
  const auto&r=ReimsGTSampler::regs[reading];
  str(" reading ");str(r.name);str(" 0x");
  for(int b=20;b>=0;b-=4)put(hex[(r.offset>>b)&0xf]);
 } else str(" starved");
 str(" for ");dec(stuck);str(" ms\n");
 if(n)out[len<n?len:n-1]='\0';
 return len;
}
}
