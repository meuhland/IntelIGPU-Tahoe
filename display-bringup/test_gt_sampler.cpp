// Host test of the read-only GT sampler format and print policy (gt_sampler.hpp).
#include <stdio.h>
#include <string.h>
#include "gt_sampler.hpp"

using namespace ReimsGTSampler;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

int main(){
 // Register list: unique, dword aligned, inside the 16 MiB BAR0.
 for(unsigned i=0;i<count;++i){
  if(regs[i].derive)continue;
  CHECK(!(regs[i].offset&3)&&regs[i].offset<0x1000000);
  for(unsigned j=i+1;j<count;++j)CHECK(regs[j].derive||regs[i].offset!=regs[j].offset);
 }
 // Derived status-page PTE: after rcsHwsPga, reads ggttBase + (hws>>12)*8 (+4),
 // and returns the marker when that is past the mapped BAR.
 {static_assert(hwsIndex>=0,"rcsHwsPga sampled");
  const int lo=indexOf("hwsPteLo"),hi=indexOf("hwsPteHi");
  CHECK(lo>hwsIndex&&hi==lo+1&&regs[lo].derive==1&&regs[hi].derive==2);
  uint32_t soFar[count]={};soFar[hwsIndex]=0x00123000;  // GGTT page 0x123
  uint32_t asked=0;auto read=[&](uint32_t o){asked=o;return 0x5a5a0001u;};
  CHECK(readColumn(unsigned(lo),soFar,0x1000000,read)==0x5a5a0001u&&asked==0x800000+0x123*8);
  CHECK(readColumn(unsigned(hi),soFar,0x1000000,read)==0x5a5a0001u&&asked==0x800000+0x123*8+4);
  // A 16 MiB BAR's GGTT half covers the whole 4 GiB GGTT, so only a shorter
  // mapping can leave the PTE outside it.
  soFar[hwsIndex]=0xfffff000;
  CHECK(readColumn(unsigned(lo),soFar,0x1000000,read)==0x5a5a0001u&&asked==0xfffff8);
  asked=0;CHECK(readColumn(unsigned(lo),soFar,0x900000,read)==outsideBar&&asked==0);
  const int p0=indexOf("ggttPte0Lo");CHECK(p0>0&&regs[p0].offset==ggttBase&&!regs[p0].derive);
  // LRCA PTE: derived from rcsSq0Lo, which precedes it.
  const int llo=indexOf("lrcaPteLo"),lhi=indexOf("lrcaPteHi");
  CHECK(sq0Index>=0&&llo>sq0Index&&lhi==llo+1&&regs[llo].derive==3&&regs[lhi].derive==4);
  soFar[sq0Index]=0x40008019;  // LRCA GGTT 0x40008000 + descriptor flags
  CHECK(readColumn(unsigned(llo),soFar,0x1000000,read)==0x5a5a0001u&&asked==0x800000+0x40008*8);
  CHECK(readColumn(unsigned(lhi),soFar,0x1000000,read)==0x5a5a0001u&&asked==0x800000+0x40008*8+4);
  CHECK(indexOf("noSuchRegister")==-1);}
 static_assert(maxOffset()==0x808004,"framebuffer PTE high dword is the highest offset");
 static_assert(regs[count-2].offset==0x800000+(0x01000000>>12)*8,"PTE of GGTT 0x01000000");

 // Line format: fixed order, 8 hex digits per register, fits the kext buffer.
 Sample s;for(unsigned i=0;i<count;++i)s.v[i]=0x11111111U*(i%16);
 s.v[0]=0x00010001;s.v[2]=0x8000f0f4;
 char line[lineBuffer];const size_t n=format(line,sizeof(line),7,1234,s);
 CHECK(n<sizeof(line)&&strlen(line)==n);
 CHECK(!strncmp(line,"ReimsGT 7 1234 00010001 11111111 8000f0f4 ",42));
 CHECK(line[n-1]=='\n');
 char head[lineBuffer];const size_t h=header(head,sizeof(head));
 CHECK(h<sizeof(head)&&!strncmp(head,"ReimsGT seq ms fwAckGT fwAckRender gucStatus ",45));
 // Worst case (max seq and ms) and the header fit the kext buffer.
 {char w[lineBuffer];CHECK(format(w,sizeof(w),0xffffffffu,~0ull,s)<sizeof(w));}
 // Truncation stays NUL-terminated.
 char tiny[16];format(tiny,sizeof(tiny),1,2,s);CHECK(strlen(tiny)==15);

 // Parts (what the kexts print): every part line, worst-case seq/ms, and every
 // header part fit IOLog's 255-character console line, and together they
 // cover each column exactly once, in order.
 {unsigned covered=0;
  for(unsigned p=0;p<parts;++p){
   char w[lineBuffer];
   CHECK(format(w,sizeof(w),0xffffffffu,~0ull,s,int(p))<consoleLine);
   CHECK(header(w,sizeof(w),int(p))<consoleLine);
   CHECK(partFirst(int(p))==covered);covered=partEnd(int(p));
  }
  CHECK(covered==count);
  CHECK(format(line,sizeof(line),7,1234,s,0)<consoleLine);
  CHECK(!strncmp(line,"ReimsGT 7 1234 @0 00010001 11111111 8000f0f4 ",45));
  format(line,sizeof(line),7,1234,s,1);
  CHECK(!strncmp(line,"ReimsGT 7 1234 @10 aaaaaaaa ",28));
  header(head,sizeof(head),1);
  CHECK(!strncmp(head,"ReimsGT seq ms @10 rcsIpehr ",28));
  // The single-line form really does exceed it (why parts exist).
  CHECK(format(line,sizeof(line),7,1234,s)>consoleLine);}

 // Print policy: first sample, changes, and a heartbeat every 20 ticks.
 State st;Policy p;Sample z;
 CHECK(st.shouldPrint(z,p));
 unsigned printed=0;
 for(unsigned i=0;i<19;++i)printed+=st.shouldPrint(z,p);
 CHECK(printed==0);
 CHECK(st.shouldPrint(z,p));           // 20th unchanged tick: heartbeat
 Sample c=z;c.v[5]=1;CHECK(st.shouldPrint(c,p));
 CHECK(!st.shouldPrint(c,p));
 // Line budget.
 State capped;Policy small;small.maxLines=2;
 Sample a,b;b.v[0]=1;
 CHECK(capped.shouldPrint(a,small)&&capped.shouldPrint(b,small)&&!capped.shouldPrint(a,small));

 if(failures)return 1;
 puts("PASS: GT sampler (register list, line format, change/heartbeat policy, budget)");return 0;
}
