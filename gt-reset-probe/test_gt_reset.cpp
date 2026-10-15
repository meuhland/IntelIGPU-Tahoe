// Host test of the GT-reset probe sequence (gt_reset.hpp): dry run writes
// nothing, the execute path forcewakes/resets/re-reads in order, and the
// populated-fuses verdict is correct. A Model simulates MMIO: forcewake acks
// after a request, GDRST self-clears, and a flag makes the geometry fuses
// appear only after the reset.
#include <cstdio>
#include <map>
#include <vector>
#include "gt_reset.hpp"

using namespace ReimsGTReset;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

struct Model {
 std::map<uint32_t,uint32_t> reg;
 std::vector<uint32_t> writes;
 bool fusesAfterReset=false;   // geometry fuses populate only after a GDRST
 bool ackForcewake=true;       // ack register follows the request
 bool resetDone=false;
 uint32_t read(uint32_t a){auto it=reg.find(a);return it==reg.end()?0u:it->second;}
 void write(uint32_t a,uint32_t v){
  writes.push_back(a);
  if((a==R::fwRenderReq||a==R::fwGtReq)){
   uint32_t ack=(a==R::fwRenderReq)?R::fwRenderAck:R::fwGtAck;
   uint32_t mask=v>>16,val=v&0xffffu;  // masked write, per requester bit
   reg[ack]=ackForcewake?((reg[ack]&~mask)|(val&mask)):0u;
  } else if(a==R::gdrst){
   reg[R::gdrst]=0; // self-clears immediately
   resetDone=true;
   if(fusesAfterReset){reg[R::sliceEnable]=1;reg[R::dssEnable]=0x3;}
  } else reg[a]=v;
 }
 void delayUS(uint32_t){}
};

int main(){
 // Dry run: raw read only, no writes.
 {Model m;m.reg[R::sliceEnable]=0;m.reg[R::dssEnable]=0;m.reg[R::mediaDisable]=0xf00ff;
  auto r=run(m,false);
  CHECK(!r.executed && m.writes.empty());
  CHECK(r.raw.media==0xf00ff && r.raw.slice==0);
  CHECK(!resetPopulatedFuses(r));}

 // Execute, reset does NOT populate (fuses stay 0): verdict false, order right.
 {Model m;m.fusesAfterReset=false;m.reg[R::mediaDisable]=0xf00ff;
  auto r=run(m,true);
  CHECK(r.executed && r.renderAcked && r.gtAcked && r.resetCleared);
  CHECK(r.afterReset.slice==0 && !resetPopulatedFuses(r));
  // Writes: 2 forcewake acquire, GDRST twice (i915 pre-MTL loop), 2 release.
  int gd=0,fw=0; for(auto a:m.writes){if(a==R::gdrst)++gd; if(a==R::fwRenderReq||a==R::fwGtReq)++fw;}
  CHECK(gd==2 && fw==4 && r.resetLoops==2);}

 // Execute, reset DOES populate: slice 0 -> 1, dss 0 -> 0x3, verdict true.
 {Model m;m.fusesAfterReset=true;m.reg[R::mediaDisable]=0xf00ff;
  auto r=run(m,true);
  CHECK(r.raw.slice==0 && r.afterReset.slice==1 && r.afterReset.dss==0x3);
  CHECK(resetPopulatedFuses(r));}

 // Forcewake ack never comes: bounded, recorded, still proceeds to reset.
 {Model m;m.ackForcewake=false;
  auto r=run(m,true);
  CHECK(!r.renderAcked && !r.gtAcked && r.executed);
  bool reset=false; for(auto a:m.writes) if(a==R::gdrst) reset=true;
  CHECK(reset);}

 // GDRST never clears: resetCleared false, bounded loops.
 {struct Stuck:Model{uint32_t read(uint32_t a){if(a==R::gdrst)return R::grdomFull;return Model::read(a);}
   void write(uint32_t a,uint32_t v){writes.push_back(a); if(a!=R::gdrst)Model::write(a,v);}};
  Stuck m; uint32_t loops=0; bool ok=gdrstReset(m,R::grdomFull,loops);
  CHECK(!ok && loops>=1);}

 // Hold: asleep GT (fuses read 0 unless the GT domain is acked) wakes under
 // requester bit 2, stays held, and coexists with Apple's bit 1 release.
 {struct Asleep:Model{uint32_t read(uint32_t a){
    if((a==R::sliceEnable||a==R::dssEnable||a==R::mediaDisable)&&!reg[R::fwGtAck])return 0;
    return Model::read(a);}};
  Asleep m;m.reg[R::sliceEnable]=1;m.reg[R::dssEnable]=3;m.reg[R::mediaDisable]=0xe00fa;
  auto h=acquireHold(m);
  CHECK(!populated(h.raw)&&h.raw.media==0);
  CHECK(h.renderAcked&&h.gtAcked&&populated(h.held)&&h.held.media==0xe00fa);
  CHECK(m.reg[R::fwGtAck]==R::fwHold);
  m.write(R::fwGtReq,(2u<<16)|2u); m.write(R::fwGtReq,(2u<<16)|0u); // Apple bit 1 on/off
  CHECK(populated(readFuses(m)));                                    // still held
  bool gd=false; for(auto a:m.writes) if(a==R::gdrst) gd=true;
  CHECK(!gd);                                                        // hold never resets
  releaseHold(m);
  CHECK(m.reg[R::fwGtAck]==0&&!populated(readFuses(m)));}

 if(failures) return 1;
 puts("PASS: GT reset probe (dry run, reset populates / not, forcewake/GDRST timeouts, hold)");
 return 0;
}
