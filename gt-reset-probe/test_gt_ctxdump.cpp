// Host test of the context-image dump helpers (gt_ctxdump.hpp).
#include <stdio.h>
#include <string.h>
#include "gt_ctxdump.hpp"

using namespace ReimsCtxDump;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

int main(){
 // LRCA and PTE arithmetic: page 1 of LRCA 0x40008000 is GGTT page 0x40009.
 CHECK(lrcaOf(0x40008019)==0x40008000);
 CHECK(pteOffset(0x40008000,0)==0x800000+0x40008*8);
 CHECK(pteOffset(0x40008000,1)==0x800000+0x40009*8);
 CHECK(pteValid(0x14b75708bull)&&!pteValid(0x14b757000ull));
 CHECK(ptePhysical(0x14b75708bull)==0x14b757000ull);
 CHECK(ptePhysical(0x3ull)==0);  // valid PTE pointing at physical 0

 // Dump once per new, nonzero LRCA.
 Tracker t;
 CHECK(!t.shouldDump(0));
 CHECK(t.shouldDump(0x40008019));
 CHECK(!t.shouldDump(0x40008011));  // same LRCA, other flags
 CHECK(t.shouldDump(0x4000c019));

 // Lines: fixed format, every one well under IOLog's 255-character limit.
 char line[ReimsGTSampler::lineBuffer];
 size_t n=formatPte(line,sizeof(line),0x40008000,1,0x14b75808bull);
 CHECK(!strcmp(line,"ReimsGT ctx 40008000 p1 pte 000000014b75808b\n")&&n<ReimsGTSampler::consoleLine);
 uint32_t v[perLine]={0x1100101b,0x2244,0x00090009,0x2034,0,0x2030,0,0x2038};
 n=formatWords(line,sizeof(line),0x40008000,1,8,v,perLine);
 CHECK(!strcmp(line,"ReimsGT ctx 40008000 p1 @8 1100101b 00002244 00090009 00002034 00000000 00002030 00000000 00002038\n"));
 uint32_t w[perLine];for(auto&x:w)x=0xffffffff;
 CHECK(formatWords(line,sizeof(line),0xfffff000,1,dwords-perLine,w,perLine)<ReimsGTSampler::consoleLine);
 CHECK(dwords%perLine==0);

 // RING_START from an LRI list, engine-relative (CS_MMIO) or absolute.
 {const uint32_t lri=(0x22u<<23)|(1u<<19)|(2*3-1);
  uint32_t p1[]={0,lri,0x244,0x00090009,0x034,0,0x038,0x40010000,0};
  uint32_t v2=0;CHECK(findLri(p1,9,lriRingStart,v2)&&v2==0x40010000);
  uint32_t p2[]={(0x22u<<23)|(2*2-1),0x2244,1,0x2038,0x40020000};
  CHECK(findLri(p2,5,lriRingStart,v2)&&v2==0x40020000);
  uint32_t p3[]={0,0,0,0};CHECK(!findLri(p3,4,lriRingStart,v2));
  uint32_t p4[]={(0x22u<<23)|(2*5-1),0x2244,1};  // truncated block: no overrun
  CHECK(!findLri(p4,3,lriRingStart,v2));}

 // GGTT window lines stay under 255.
 {uint64_t ptes[gttPerLine];for(auto&p:ptes)p=0x7fffffffffffffffull;
  char l2[ReimsGTSampler::lineBuffer];
  CHECK(formatGtt(l2,sizeof(l2),gttWindow,ptes,gttPerLine)<ReimsGTSampler::consoleLine);
  uint64_t two[2]={0x14b75708bull,0x3ull};
  formatGtt(l2,sizeof(l2),0x40004000,two,2);
  CHECK(!strcmp(l2,"ReimsGT gtt 40004000 000000014b75708b 0000000000000003\n"));
 }

 if(failures)return 1;
 puts("PASS: context dump (LRCA/PTE math, once per LRCA, line format under 255)");
 return 0;
}
