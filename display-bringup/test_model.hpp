#pragma once
// Register model for the host-side bring-up tests. Registers the sequences
// wait on follow their request bits; everything else stores what is written.
// Initial state: the guest idle state measured by the phase 4 probe, with
// combo PHY COMP_DW3 codes as read on the host.
#include <map>
#include <vector>
#include "core_init.hpp"

namespace ReimsBringupTest {
using namespace ReimsBringup;
struct Model {
 std::map<uint32_t,uint32_t> r;std::vector<std::pair<uint32_t,uint32_t>> writes;
 std::vector<uint32_t> pcodeCalls;uint64_t now=0;
 bool pwAck=true,fuseAck=true,pllLocks=true,dbufAck=true,pcodeHangs=false;
 bool ddiIoAck=true,dpllLocks=true,ddiBufActive=true,surfLatches=true,scdcNak=false;
 unsigned writes64=0;uint8_t scdcTmds=0;uint32_t gmbus3=0;
 unsigned pcodeBusyReplies=0;uint32_t pcodeStatus=0;
 Model(){
  r[R::fuseStatus]=0x88000000;r[R::dssm]=0x40000020;r[R::dcStateEn]=0;
  r[R::pwrWellDriver]=0;r[R::cdclkPll]=0;r[R::cdclkCtl]=0x00380000;
  for(unsigned p=0;p<5;++p){const uint32_t b=R::phyBase[p];
   r[b+R::compDw3]=(p==0||p==3)?0xc0606321:0xc0608021;r[b+R::txDw8Ln0]=0x00001234;r[b+R::pcsDw1Ln0]=0x00300000|0x55;}
  r[R::phyMiscA]=R::deIoCompPwrDown;
  r[0x164280]=0x01e07c00;r[0x1642bc]=0x00000030;   // all DDI clocks gated (probe)
  r[0x64300]=1U<<7;                                // DDI buffer TC1 idle
 }
 uint32_t read(uint32_t a){
  auto i=r.find(a);uint32_t v=i==r.end()?0:i->second;
  // PIPEDSL A advances while transcoder A runs.
  if(a==0x70000&&(r[0x70008]&(1U<<30)))r[a]=v=(v+7)%2250;
  if(a==0xc510c)return gmbus3;            // GMBUS3: staged SCDC read byte
  if(a==0xc5108)return r[0xc5108];        // GMBUS2
  return v;
 }
 void delayUS(uint32_t us){now+=us;}
 uint64_t nowUS(){return now;}
 void write(uint32_t a,uint32_t v){
  writes.push_back({a,v});now+=1;
  if(a==R::pwrWellDriver){
   v=(v&~0x15555U)|(pwAck?(v>>1)&0x15555U:0);
   if(fuseAck){
    if(v&R::pw1State)r[R::fuseStatus]|=R::fusePG1;
    if(v&(1U<<2))r[R::fuseStatus]|=1U<<25;          // PW2 -> PG2
    if(v&(1U<<4))r[R::fuseStatus]|=1U<<24;          // PW3 -> PG3
   }
  }else if(a==0x45454){                            // DDI IO power: state follows request
   v=(v&~0x5555U)|(ddiIoAck?(v>>1)&0x5555U:0);
  }else if(a==0x46010){                            // DPLL0 enable
   v&=~(1U<<30|1U<<26);
   if(v&(1U<<27))v|=1U<<26;
   if(dpllLocks&&(v&(1U<<31)))v|=1U<<30;
  }else if(a==0x70008){                            // TRANSCONF A state follows enable
   v=(v&~(1U<<30))|((v&(1U<<31))?1U<<30:0);
  }else if(a==0x7019c){                            // PLANE_SURF 1A latches on a running pipe
   if(surfLatches&&(r[0x70180]&(1U<<31))&&(r[0x70008]&(1U<<30)))r[0x701ac]=v;
  }else if(a==0xc510c){                            // GMBUS3 payload latch
   gmbus3=v;return;
  }else if(a==0xc5104){                            // GMBUS1 cycle
   if(v&(4U<<25))return;                           // STOP
   if(scdcNak){r[0xc5108]|=1U<<10;return;}         // SATOER
   if(v&1U){                                       // indexed read
    const uint8_t off=(v>>8)&0xff;
    gmbus3 = off==0x20?scdcTmds : off==0x21?uint8_t((scdcTmds&1)?1:0):0;
    r[0xc5108]|=1U<<11;                            // HW_RDY
   }else{                                          // 2-byte write [off,val]
    if((gmbus3&0xff)==0x20)scdcTmds=(gmbus3>>8)&0xff;
    r[0xc5108]|=1U<<11;
   }
   return;
  }else if(a==0xc5100||a==0xc5110){return;         // GMBUS0/GMBUS4: no-op
  }else if(a==0x64300){                            // DDI buffer TC1 idle until enabled
   v=(v&~(1U<<7))|((ddiBufActive&&(v&(1U<<31)))?0:1U<<7);
  }else if(a==R::cdclkPll){
   v=(v&~R::pllLock)|((pllLocks&&(v&R::pllEnable))?R::pllLock:0);
  }else if(a==R::dbufCtl[0]||a==R::dbufCtl[1]){
   v=(v&~R::dbufState)|((dbufAck&&(v&R::dbufRequest))?R::dbufState:0);
  }else if(a==R::pcodeMailbox&&(v&R::pcodeReady)){
   pcodeCalls.push_back(r[R::pcodeData]);
   if(pcodeHangs){r[a]=v;return;}
   if((v&0xff)==R::pcodeCdclkControl&&r[R::pcodeData]==R::cdclkPrepare){
    const bool busy=pcodeBusyReplies>0;if(busy)--pcodeBusyReplies;
    r[R::pcodeData]=busy?0:R::cdclkReady;
   }
   v=(v&~R::pcodeReady&~R::pcodeErrorMask)|pcodeStatus;
  }
  // PHY group registers land in every lane (TX DW5/DW8, PCS DW1).
  for(unsigned p=0;p<5;++p){
   const uint32_t b=R::phyBase[p];
   for(unsigned ln=0;ln<4;++ln){
    if(a==b+R::txDw8Grp)r[b+0x880+ln*0x100+0x20]=v;
    if(a==b+0x694)r[b+0x880+ln*0x100+0x14]=v;
    if(a==b+R::pcsDw1Grp)r[b+0x800+ln*0x100+0x4]=v;
   }
  }
  r[a]=v;
 }
 // GGTT entries (BAR0 + 8 MiB) are written as one 64-bit store.
 void write64(uint32_t a,uint64_t v){++writes64;now+=1;r[a]=uint32_t(v);r[a+4]=uint32_t(v>>32);}
 bool wrote(uint32_t a)const{for(auto&w:writes)if(w.first==a)return true;return false;}
};
}
