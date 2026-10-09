// Host-side test of hdmi_pll.hpp: matches host i915's DPLL0 values for
// 4K60 and round-trips through the independent display_timing.hpp decoder.
// Built by scripts/test.sh with -DREIMS_TARGET_RPLS.
#include <stdio.h>
#include "hdmi_pll.hpp"
#include "../desktop-reset-recovery-20260917/source/desktop-link/display_timing.hpp"

static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

using namespace ReimsDisplayTiming;
static void set(Snapshot&s,uint32_t a,uint32_t v){
 for(unsigned i=0;i<count;++i)if(offsets[i]==a){s.value[i]=v;return;}
 printf("FAIL register 0x%x not captured\n",a);++failures;
}
// Port clock the decoder derives from these CFGCR values on HDMI TC1/DPLL0.
static uint64_t decodedPortHz(const ReimsHdmiPll::Cfg&c){
 Snapshot s;
 set(s,0x51004,0x40000020);set(s,0x46010,0xcc000000);set(s,0x46140,0x40000000);
 set(s,0x164280,0x01e07400);set(s,0x164284,c.cfgcr0);set(s,0x164288,c.cfgcr1);
 set(s,0x60000,0x112f0eff);set(s,0x60008,0x10070faf);set(s,0x6000c,0x08c9086f);set(s,0x60014,0x08810877);
 set(s,0x60400,0x80000000|4U<<27);set(s,0x70008,0xc0000000);
 Mode m;return decode(s,0,m)==Status::OK?m.portClockHz:0;
}

int main(){
 // Host i915 on this machine: 3840x2160@60, DPLL0 cfgcr0 0x1001d0, cfgcr1 0x448.
 auto c=ReimsHdmiPll::compute(594000);
 CHECK(c.ok&&c.cfgcr0==0x001001d0&&c.cfgcr1==0x00000448&&c.dcoKHz==8910000);
 // First guest modes: same DCO, different dividers.
 auto fhd=ReimsHdmiPll::compute(148500);CHECK(fhd.ok&&fhd.cfgcr0==0x001001d0&&fhd.cfgcr1==0x00000e84);
 auto uhd30=ReimsHdmiPll::compute(297000);CHECK(uhd30.ok&&uhd30.cfgcr0==0x001001d0&&uhd30.cfgcr1==0x00000488);
 // Every common HDMI clock round-trips through the decoder within 1 kHz.
 const uint32_t clocks[]={25175,27000,74250,108000,119000,148500,154000,241500,297000,340000,594000};
 for(uint32_t k:clocks){
  auto p=ReimsHdmiPll::compute(k);CHECK(p.ok);
  const uint64_t hz=decodedPortHz(p),want=uint64_t(k)*1000;
  CHECK(hz+1000>=want&&hz<=want+1000);
  if(!(hz+1000>=want&&hz<=want+1000))printf("  %u kHz -> %llu Hz (cfgcr0 %#x cfgcr1 %#x)\n",k,(unsigned long long)hz,p.cfgcr0,p.cfgcr1);
 }
 // Clocks with no divider in range are refused.
 CHECK(!ReimsHdmiPll::compute(1000).ok);
 if(failures){printf("%d failure(s)\n",failures);return 1;}
 printf("PASS: HDMI DPLL computation (host 4K60 match, decoder round trip)\n");
 return 0;
}
