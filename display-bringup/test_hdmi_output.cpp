// Host-side test of stage 3a (hdmi_output.hpp): stage 2 + HDMI TC1 at 4K30
// from the guest idle state, checked against host i915's register values.
// Built by scripts/test.sh with -DREIMS_DISPLAY_BRINGUP=1 -DREIMS_TARGET_RPLS.
#include <stdio.h>
#include "test_model.hpp"
#include "hdmi_output.hpp"

using namespace ReimsBringup;
using ReimsBringupTest::Model;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)
static bool fieldIs(uint32_t v,uint32_t mask,uint32_t want){return (v&mask)==want;}
static bool loggedFail(const HdmiOutput<Model>&o,Step s){
 for(unsigned i=0;i<o.count;++i)if(o.log[i].step==uint8_t(s)&&o.log[i].action==uint8_t(Action::Fail))return true;
 return false;
}

int main(){
 // Dry run plans stage 2 and 3a and touches nothing.
 {Model m;HdmiOutput<Model> o(m,false);
  CHECK(o.runOutput(kUhd30)==Result::Planned);CHECK(m.writes.empty());}
 // Full run: host reference values, with 4K30's PLL divider, linetime and
 // no scrambling bits.
 {Model m;HdmiOutput<Model> o(m,true);
  CHECK(o.runOutput(kUhd30)==Result::OK);
  CHECK((m.read(0x45404)&0x3f)==0x3f);                       // PW1-3 requested and on
  CHECK((m.read(0x42000)&0x0f000000)==0x0f000000);           // PG0-3 distributed
  CHECK(m.read(0x46010)==0xcc000000);                        // DPLL0 like the host
  CHECK(m.read(0x164284)==0x001001d0&&m.read(0x164288)==0x00000488);
  CHECK(fieldIs(m.read(0x164280),3U<<2|1U<<11,0));           // PHY B <- DPLL0, ungated
  CHECK(fieldIs(m.read(0x45454),0xc0,0xc0));                 // DDI IO TC1, as host 0xc0
  CHECK(m.read(0x46140)==0x40000000);
  // AVI infoframe enabled, 4K30 full-range packet in the DIP data registers.
  CHECK(m.read(0x60200)==(1u<<12));
  CHECK(m.read(0x60220)==0x000d0282);            // type/version/length/hole
  CHECK(m.read(0x60224)==0x082812ce);            // cksum CE, data0 0x12, data1 0x28, data2 0x08 (Q full)
  CHECK(m.read(0x60228)==0x0000005f);            // data3 = VIC 95 (4K30), rest 0
  CHECK(m.read(0x6001c)==0x0eff086f&&m.read(0x70030)==0x00800100);
  CHECK(m.read(0x60000)==0x112f0eff&&m.read(0x60004)==0x112f0eff&&m.read(0x60008)==0x10070faf);
  CHECK(m.read(0x6000c)==0x08c9086f&&m.read(0x60010)==0x08c9086f&&m.read(0x60014)==0x08810877);
  CHECK(m.read(0x60028)==0&&m.read(0x6002c)==0&&fieldIs(m.read(0x420c0),3U<<27,0));
  CHECK(m.read(0x45270)==119);                               // host 60 at 594 MHz
  CHECK(fieldIs(m.read(0x70038),0x8080,0x8080));
  CHECK(fieldIs(m.read(0x7003c),R3::mbusDboxMask,0x01038c02)); // host 0xb1038c02: bits 31:28 not written by i915
  CHECK(m.read(0x60400)==0xa0030000&&m.read(0x60404)==0);   // host 0xa0030011 minus scrambling
  CHECK(m.read(0x70008)==0xc0000000);
  CHECK(m.read(0x64300)==0x80000000);                        // host DDI_BUF_CTL TC1
  for(unsigned ln=0;ln<4;++ln){                              // host PHY B lanes
   const uint32_t t=0x6c880+ln*0x100;
   CHECK(fieldIs(m.read(t+0x8),1U<<15|7U<<11|0xff,0x3098));
   CHECK(fieldIs(m.read(t+0x10),1U<<31|0x3ffff,(ln?1U<<31:0)|0xa035));
   CHECK(fieldIs(m.read(t+0x14),0xe61c0038,0xa0080030));      // training, tap2/3, cursor/polarity, scaling 2, rterm 6
   CHECK(fieldIs(m.read(t+0x1c),0x7fU<<24,0x7fU<<24));
  }
  CHECK(fieldIs(m.read(0x6c014),3,3)&&fieldIs(m.read(0x6c028),0xf0,0));
  CHECK(fieldIs(m.read(0x6c804),1U<<26,0));
  CHECK(o.log[o.count-1].step==uint8_t(Step::Done));}
 // Refusals and stops.
 {Model m;HdmiOutput<Model> o(m,true);
  CHECK(o.runOutput({1920,2008,2052,2200,1080,1084,1089,1125,594000,60,true,true})==Result::PreconditionFailed);
  CHECK(!m.wrote(0x46010));}
 {Model m;m.r[0x70008]=0xc0000000;HdmiOutput<Model> o(m,true);   // pipe already running
  CHECK(o.runOutput(kUhd30)==Result::PreconditionFailed);CHECK(!m.wrote(0x46010)&&!m.wrote(0x60400));}
 {Model m;m.r[0x46010]=0xcc000000;m.r[0x164284]=0x018001d4;HdmiOutput<Model> o(m,true); // DPLL0 busy elsewhere
  CHECK(o.runOutput(kUhd30)==Result::Unexpected);CHECK(!m.wrote(0x164284)&&!m.wrote(0x164280));}
 {Model m;m.dpllLocks=false;HdmiOutput<Model> o(m,true);
  CHECK(o.runOutput(kUhd30)==Result::Timeout);CHECK(loggedFail(o,Step::DpllEnable));CHECK(!m.wrote(0x164280));}
 {Model m;m.ddiIoAck=false;HdmiOutput<Model> o(m,true);
  CHECK(o.runOutput(kUhd30)==Result::Timeout);CHECK(loggedFail(o,Step::DdiIoPower));CHECK(!m.wrote(0x60400));}
 {Model m;m.ddiBufActive=false;HdmiOutput<Model> o(m,true);
  CHECK(o.runOutput(kUhd30)==Result::Timeout);CHECK(loggedFail(o,Step::DdiBuf));}
 {Model m;m.fuseAck=false;HdmiOutput<Model> o(m,true);            // PG1 never distributes: stage 2 stops
  CHECK(o.runOutput(kUhd30)==Result::Timeout);CHECK(!m.wrote(0x46010));}
 if(failures){printf("%d failure(s)\n",failures);return 1;}
 printf("PASS: RPL-S HDMI TC1 4K30 output (host reference values, refusals, timeouts)\n");
 return 0;
}
