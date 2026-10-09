// Host-side test of stage 3b (hdmi_scanout.hpp): stage 2 + 3a + plane 1A
// scanning out a GGTT-mapped 3840x2160 framebuffer, checked against host
// i915's plane registers. Built by scripts/test.sh with
// -DREIMS_DISPLAY_BRINGUP=1 -DREIMS_TARGET_RPLS.
#include <stdio.h>
#include <vector>
#include "test_model.hpp"
#include "hdmi_scanout.hpp"

using namespace ReimsBringup;
using ReimsBringupTest::Model;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

int main(){
 std::vector<uint64_t> dma(R4::pages);
 for(uint32_t i=0;i<R4::pages;++i)dma[i]=0x123400000ULL+uint64_t(i)*0x1000*3; // scattered pages
 const Framebuffer fb{0x01000000,R4::pages,dma.data()};
 // Dry run plans everything and touches nothing.
 {Model m;HdmiScanout<Model> s(m,false);
  CHECK(s.runScanout(kUhd30,fb)==Result::Planned);CHECK(m.writes.empty()&&m.writes64==0);}
 // Full run.
 {Model m;HdmiScanout<Model> s(m,true);
  CHECK(s.runScanout(kUhd30,fb)==Result::OK);
  CHECK((m.read(0x44fe8)&(3U<<30))==(3U<<30));                  // DBUF S2 like the host
  CHECK(m.read(0x4a480)==0&&m.read(0x49028)==0);                // full range, no LUT
  CHECK(m.writes64==R4::pages);
  bool ptes=true;
  for(uint32_t i=0;i<R4::pages;++i){
   const uint32_t a=0x800000+(fb.ggttOffset>>12)*8+i*8;
   ptes&=m.read(a)==uint32_t(dma[i]|1)&&m.read(a+4)==uint32_t(dma[i]>>32);
  }
  CHECK(ptes);
  // Host plane 1A values for the same framebuffer layout.
  CHECK(m.read(0x70180)==0x84000000&&m.read(0x70188)==0xf0&&m.read(0x70190)==0x086f0eff);
  CHECK(m.read(0x7018c)==0&&m.read(0x701a4)==0&&m.read(0x701a0)==0xff000000&&m.read(0x701cc)==0x2000);
  const uint32_t wm[]={0x8000401f,0x8000c05e,0x8001409c,0x8001409c,0x8001409c,0x8001c0da,0x800381b3,0x8005026d};
  for(unsigned l=0;l<8;++l)CHECK(m.read(0x70240+4*l)==wm[l]);
  CHECK(m.read(0x70268)==0x8000002d&&m.read(0x7027c)==0x07ba0000);
  CHECK(m.read(0x7019c)==fb.ggttOffset&&m.read(0x701ac)==fb.ggttOffset);
  // PLANE_CTL is written immediately before PLANE_SURF.
  size_t ctl=0,surf=0;for(size_t i=0;i<m.writes.size();++i){if(m.writes[i].first==0x70180)ctl=i;if(m.writes[i].first==0x7019c)surf=i;}
  CHECK(surf==ctl+1);
  CHECK(s.log[s.count-1].step==uint8_t(Step::Done));
  s.disablePlane();CHECK(m.read(0x70180)==0&&m.read(0x7019c)==0);}
 // Refusals before any register write.
 auto refused=[&](const Framebuffer&f,uint32_t planeCtl){
  Model m;m.r[0x70180]=planeCtl;HdmiScanout<Model> s(m,true);
  // Refused before stage 2 or 3a writes anything.
  const bool ok=s.runScanout(kUhd30,f)==Result::PreconditionFailed&&m.writes.empty()&&m.writes64==0;
  return ok;
 };
 CHECK(refused({0x01001000,R4::pages,dma.data()},0));          // not 256 KiB aligned
 CHECK(refused({0x01000000,R4::pages-1,dma.data()},0));        // wrong size
 CHECK(refused({0x01000000,R4::pages,nullptr},0));             // no pages
 {std::vector<uint64_t> bad=dma;bad[17]=0x123400800ULL;CHECK(refused({0x01000000,R4::pages,bad.data()},0));} // unaligned page
 CHECK(refused(fb,0x84000000));                                // plane already on
 {Model m;HdmiScanout<Model> s(m,true);                        // 3a mode must be 3840x2160
  CHECK(s.runScanout({1920,2008,2052,2200,1080,1084,1089,1125,148500,60,true,true,false},fb)==Result::PreconditionFailed);
  CHECK(m.writes.empty());}
 // Timeouts.
 {Model m;m.surfLatches=false;HdmiScanout<Model> s(m,true);
  CHECK(s.runScanout(kUhd30,fb)==Result::Timeout);}
 {Model m;m.dbufAck=false;HdmiScanout<Model> s(m,true);       // stage 2 S1 already fails
  CHECK(s.runScanout(kUhd30,fb)==Result::Timeout);CHECK(m.writes64==0);}
 if(failures){printf("%d failure(s)\n",failures);return 1;}
 printf("PASS: RPL-S plane 1A scanout over HDMI TC1 (GGTT, host plane values, refusals)\n");
 return 0;
}
