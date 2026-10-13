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
  CHECK(!(regs[i].offset&3)&&regs[i].offset<0x1000000);
  for(unsigned j=i+1;j<count;++j)CHECK(regs[i].offset!=regs[j].offset);
 }
 static_assert(maxOffset()==0x808004,"framebuffer PTE high dword is the highest offset");
 static_assert(regs[count-2].offset==0x800000+(0x01000000>>12)*8,"PTE of GGTT 0x01000000");

 // Line format: fixed order, 8 hex digits per register, fits the kext buffer.
 Sample s;for(unsigned i=0;i<count;++i)s.v[i]=0x11111111U*(i%16);
 s.v[0]=0x00010001;s.v[2]=0x8000f0f4;
 char line[320];const size_t n=format(line,sizeof(line),7,1234,s);
 CHECK(n<sizeof(line)&&strlen(line)==n);
 CHECK(!strncmp(line,"ReimsGT 7 1234 00010001 11111111 8000f0f4 ",42));
 CHECK(line[n-1]=='\n');
 char head[320];const size_t h=header(head,sizeof(head));
 CHECK(h<sizeof(head)&&!strncmp(head,"ReimsGT seq ms fwAckGT fwAckRender gucStatus ",45));
 // Truncation stays NUL-terminated.
 char tiny[16];format(tiny,sizeof(tiny),1,2,s);CHECK(strlen(tiny)==15);

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
