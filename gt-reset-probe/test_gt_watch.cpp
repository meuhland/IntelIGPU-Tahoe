// Host test of the hold build's GT watch policy (gt_watch.hpp): new samples
// print through ReimsGTSampler's policy, a sampler that stops advancing is
// reported as stuck in a named register read or as starved, and stall lines
// repeat every 5 s, not every tick.
#include <stdio.h>
#include <string.h>
#include "gt_watch.hpp"

using namespace ReimsGTWatch;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

int main(){
 ReimsGTSampler::Sample s;
 {Reporter r;uint64_t t=0;
  CHECK(r.tick(t,0,-1,s)==none);              // nothing published yet
  t+=250;CHECK(r.tick(t,1,-1,s)==sample);     // first sample prints
  t+=250;CHECK(r.tick(t,2,-1,s)==none);       // unchanged, no heartbeat yet
  s.v[3]=0x40;t+=250;CHECK(r.tick(t,3,-1,s)==sample);  // a change prints
  // Sampler stuck reading register 5 (rcsHead): silent for < 2 s, then a
  // stall, then repeated only every 5 s.
  unsigned stalls=0;
  for(int i=0;i<40;++i){t+=250;if(r.tick(t,3,5,s)&stall)++stalls;}  // 10 s
  CHECK(stalls==2);
  CHECK(r.stuckMS(t)==10000);
  // It recovers: the next sample clears the stall and is evaluated normally.
  t+=250;s.v[3]=0x41;CHECK(r.tick(t,4,-1,s)==sample);
  t+=250;CHECK(r.tick(t,4,-1,s)==none);}

 {char line[ReimsGTSampler::lineBuffer];
  const size_t n=formatStall(line,sizeof(line),12,34567,5,2250);
  CHECK(n<sizeof(line)&&line[n-1]=='\n');
  CHECK(!strcmp(line,"ReimsGT stall seq 12 ms 34567 reading rcsHead 0x002034 for 2250 ms\n"));
  formatStall(line,sizeof(line),12,34567,-1,3000);
  CHECK(!strcmp(line,"ReimsGT stall seq 12 ms 34567 starved for 3000 ms\n"));
  char tiny[10];formatStall(tiny,sizeof(tiny),1,2,0,3);CHECK(strlen(tiny)==9);}

 if(failures)return 1;
 puts("PASS: GT watch (sample policy, stuck read vs starved, stall repeat)");
 return 0;
}
