// Host-side test of the stage 2 core init against a register model. Built by
// scripts/test.sh with -DREIMS_DISPLAY_BRINGUP=1 -DREIMS_TARGET_RPLS.
#include <stdio.h>
#include "test_model.hpp"

using namespace ReimsBringup;
using ReimsBringupTest::Model;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

static unsigned countAction(const CoreInit<Model>&c,Action a){
 unsigned n=0;for(unsigned i=0;i<c.count;++i)n+=c.log[i].action==uint8_t(a);return n;
}

int main(){
 // Dry run plans every write and touches nothing.
 {Model m;CoreInit<Model> c(m,false);
  CHECK(c.run()==Result::Planned);CHECK(m.writes.empty());
  CHECK(countAction(c,Action::Plan)>=20);CHECK(countAction(c,Action::Fail)==0);
  CHECK(c.log[c.count-1].step==uint8_t(Step::Done));}
 // Full run from the idle guest state reaches the host reference values.
 {Model m;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::OK);CHECK(countAction(c,Action::Fail)==0);
  CHECK(m.read(R::southClockGate)&R::dpmgunitGate);
  CHECK(m.read(R::rstwrnOpt)&R::pchHandshake);
  CHECK(!(m.read(R::phyMiscA)&R::deIoCompPwrDown));
  for(unsigned p=0;p<5;++p){
   const uint32_t b=R::phyBase[p];
   CHECK(m.read(b+R::compDw0)&R::compInit);CHECK(m.read(b+R::clDw5)&R::clPowerDownEnable);
   CHECK(m.read(b+R::compDw9)==0x62AB67BB&&m.read(b+R::compDw10)==0x51914F96); // 0.85V dot0, as on the host
   CHECK(bool(m.read(b+R::compDw8)&R::irefgen)==(p==0||p==3));
   CHECK((m.read(b+R::txDw8Ln0)&(R::odccClkSel|R::odccDivMask))==(R::odccClkSel|R::odccDiv2));
   CHECK(!(m.read(b+R::pcsDw1Ln0)&R::dccModeMask)&&(m.read(b+R::pcsDw1Ln0)&0x55)==0x55);
  }
  CHECK((m.read(R::pwrWellDriver)&3)==3&&(m.read(R::fuseStatus)&R::fusePG1));
  CHECK(m.read(R::cdclkPll)==0xc0000010);           // host reference
  CHECK(m.read(R::cdclkCtl)==0x00380264);           // host reference
  CHECK(m.pcodeCalls.size()==2&&m.pcodeCalls[0]==R::cdclkPrepare&&m.pcodeCalls[1]==cdclkVoltage);
  for(uint32_t d:R::dbufCtl)CHECK(((m.read(d)>>19)&0x1f)==8);
  CHECK((m.read(R::dbufCtl[0])&(R::dbufRequest|R::dbufState))==(R::dbufRequest|R::dbufState));
  CHECK(!(m.read(R::dbufCtl[1])&R::dbufRequest));   // S2 left to the modeset
  for(uint32_t a:R::mbusAbox)CHECK((m.read(a)&0x3f1f1f)==0x00111010);
  for(uint32_t a:R::bwBuddyCtl)CHECK(m.read(a)==R::bwBuddyDisable);
  CHECK((m.read(R::chickenDcpr2)&R::dcprWa)==R::dcprWa);
  // A second run finds everything in place and writes nothing.
  m.writes.clear();m.pcodeCalls.clear();CoreInit<Model> again(m,true);
  CHECK(again.run()==Result::OK);CHECK(m.writes.empty());CHECK(m.pcodeCalls.empty());
  CHECK(countAction(again,Action::Write)==0);}
 // pcode answers "not ready" a few times before accepting.
 {Model m;m.pcodeBusyReplies=5;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::OK);CHECK(m.pcodeCalls.size()==7);}
 // Other procmon codes select their own table row (0.95V dot0 here).
 {Model m;for(uint32_t b:R::phyBase)m.r[b+R::compDw3]=1U<<24;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::OK);
  for(uint32_t b:R::phyBase)CHECK(m.read(b+R::compDw9)==0x86E172C7&&m.read(b+R::compDw10)==0x77CA5EAB);}
 // A CDCLK PLL left at another ratio is disabled before relocking at 16.
 {Model m;m.r[R::cdclkPll]=R::pllEnable|R::pllLock|9;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::OK);CHECK(m.read(R::cdclkPll)==0xc0000010);
  bool sawDisable=false;for(auto&w:m.writes)sawDisable|=w.first==R::cdclkPll&&!(w.second&R::pllEnable)&&(w.second&0xff)==9;
  CHECK(sawDisable);}
 // Failures stop the sequence before later stages touch anything.
 {Model m;m.r[R::dssm]=0;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::PreconditionFailed);CHECK(m.writes.empty());}
 {Model m;m.r[R::dcStateEn]=2;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::PreconditionFailed);CHECK(m.writes.empty());}
 {Model m;m.r[R::fuseStatus]=0;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::PreconditionFailed);CHECK(m.writes.empty());}
 {Model m;m.r[R::phyBase[0]+R::compDw3]=3U<<24;CoreInit<Model> c(m,true); // unknown voltage
  CHECK(c.run()==Result::Unexpected);CHECK(!m.wrote(R::phyMiscA)&&!m.wrote(R::pwrWellDriver));}
 {Model m;m.pwAck=false;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::Timeout);CHECK(!m.wrote(R::pcodeMailbox)&&!m.wrote(R::cdclkPll));
  CHECK(m.now<5000);}
 {Model m;m.fuseAck=false;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::Timeout);CHECK(!m.wrote(R::cdclkPll));}
 {Model m;m.pcodeHangs=true;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::Timeout);CHECK(!m.wrote(R::cdclkPll));CHECK(m.now<60000);}
 {Model m;m.pcodeStatus=0x11;CoreInit<Model> c(m,true);              // GEN11_PCODE_REJECTED
  CHECK(c.run()==Result::Unexpected);CHECK(!m.wrote(R::cdclkPll));}
 {Model m;m.pllLocks=false;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::Timeout);CHECK(!m.wrote(R::cdclkCtl));CHECK(m.pcodeCalls.size()==1);}
 {Model m;m.dbufAck=false;CoreInit<Model> c(m,true);
  CHECK(c.run()==Result::Timeout);CHECK(!m.wrote(R::mbusAbox[0]));}
 if(failures){printf("%d failure(s)\n",failures);return 1;}
 printf("PASS: RPL-S display core init (dry run, full run, idempotence, failure stops)\n");
 return 0;
}
