// Host-side test of hdmi_scdc.hpp: a GMBUS+SCDC register model holds the
// sink's TMDS_CONFIG (0x20) and scrambler status (0x21); setup() must leave
// both the 1/40 ratio and scrambling enabled, and report status. NAK handled.
#include <stdio.h>
#include "hdmi_scdc.hpp"
using namespace ReimsSCDC;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)
namespace G=ReimsEDID::R;
// GMBUS+SCDC register model: records the last GMBUS3 dword so a 2-byte
// write [offset,value] and an indexed 1-byte read both resolve correctly.
struct Model2 {
 uint8_t tmds=0x00; uint32_t g2=0,g3=0; uint64_t now=0; bool nak=false; uint8_t readVal=0;
 uint64_t nowUS(){return now;} void delayUS(uint32_t us){now+=us;}
 uint32_t read(uint32_t a){ if(a==G::gmbus2)return g2; if(a==G::gmbus3)return readVal; return 0; }
 void write(uint32_t a,uint32_t v){
  if(a==G::gmbus3){g3=v;return;}
  if(a==G::gmbus1){
   if(v&G::cycleStop)return;
   if(nak){g2|=G::satoer;return;}
   if(v&G::slaveRead){
    const uint8_t off=(v>>G::slaveIndexShift)&0xff;
    readVal = off==kTmdsConfig?tmds : off==kScramblerStatus?uint8_t((tmds&kScramblingEnable)?kScramblingStatus:0):0;
    g2|=G::hwRdy;
   }else{ // 2-byte write: g3 = offset | value<<8
    const uint8_t off=g3&0xff,val=(g3>>8)&0xff;
    if(off==kTmdsConfig)tmds=val;
    g2|=G::hwRdy;
   }
  }
 }
};
int main(){
 // 4K60 setup: 1/40 ratio + scrambling both enabled, status confirms.
 {Model2 m;Scdc<Model2> s(m);
  CHECK(s.setup(2,true,true)==Status::OK);
  CHECK(m.tmds==(kBitClockRatioBy40|kScramblingEnable));
  CHECK(s.scramblingStatus(2));}
 // Disabling clears both bits.
 {Model2 m;m.tmds=0x03;Scdc<Model2> s(m);
  CHECK(s.setup(2,false,false)==Status::OK);CHECK(m.tmds==0);CHECK(!s.scramblingStatus(2));}
 // A sink that NAKs reports Nak, not success.
 {Model2 m;m.nak=true;Scdc<Model2> s(m);CHECK(s.setup(2,true,true)==Status::Nak);}
 if(failures){printf("%d failure(s)\n",failures);return 1;}
 printf("PASS: SCDC setup over GMBUS (1/40 ratio + scrambling, disable, NAK)\n");
 return 0;
}
