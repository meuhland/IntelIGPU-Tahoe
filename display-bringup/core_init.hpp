#pragma once
#include <stdint.h>

#if !defined(REIMS_DISPLAY_BRINGUP) || !defined(REIMS_TARGET_RPLS)
#error "display bring-up writes RPL-S registers: build with REIMS_DISPLAY_BRINGUP=1 REIMS_TARGET=rpls"
#endif

// Phase 5 stage 2: RPL-S display core init, ported from Linux i915 v7.2
// icl_display_core_init for ADL-S (display 12.00) with an ADP PCH and the
// 38.4 MHz reference measured on the target. See docs/PHASE5-DISPLAY-BRINGUP.md.
// Each step first checks whether the hardware is already in its target state
// and skips if so. With execute=false nothing is written: every write is
// logged as planned. The first failed check or timeout stops the sequence.
// IO provides uint32_t read(uint32_t), void write(uint32_t,uint32_t),
// void delayUS(uint32_t) and uint64_t nowUS() (monotonic); timeouts use nowUS.
namespace ReimsBringup {
enum class Step:uint8_t {
 Preconditions=1,DCOff,WaPchClockGate,PchHandshake,ComboPhy,PowerWell1,
 CdclkPrepare,CdclkPll,CdclkCtl,CdclkVoltage,DbufTracker,DbufPower,Mbus,
 BwBuddy,WaDcpr,Done};
enum class Action:uint8_t {Check,Skip,Plan,Write,Fail};
struct Entry {uint8_t step,action,index,pad;uint32_t reg,before,after;};
enum class Result:uint32_t {OK,Planned,PreconditionFailed,Timeout,Unexpected,LogFull};

// Registers (Linux i915 v7.2 headers).
namespace R {
constexpr uint32_t fuseStatus=0x42000;       // SKL_FUSE_STATUS
constexpr uint32_t fusePG0=1U<<27,fusePG1=1U<<26;
constexpr uint32_t dssm=0x51004;             // SKL_DSSM, ref clock 31:29
constexpr uint32_t dcStateEn=0x45504;        // DC_STATE_EN
constexpr uint32_t dcStateMask=0x3|1U<<3|1U<<30; // DC5/6, DC9, DC3CO
constexpr uint32_t southClockGate=0xc2020;   // SOUTH_DSPCLK_GATE_D
constexpr uint32_t dpmgunitGate=1U<<15;      // PCH_DPMGUNIT_CLOCK_GATE_DISABLE
constexpr uint32_t rstwrnOpt=0x46408;        // HSW_NDE_RSTWRN_OPT
constexpr uint32_t pchHandshake=1U<<4;       // RESET_PCH_HANDSHAKE_ENABLE
constexpr uint32_t pwrWellDriver=0x45404;    // HSW_PWR_WELL_CTL2, PW_1 idx 0
constexpr uint32_t pw1Request=0x2,pw1State=0x1;
constexpr uint32_t cdclkCtl=0x46000;         // CDCLK_CTL
constexpr uint32_t cdclkPll=0x46070;         // BXT_DE_PLL_ENABLE
constexpr uint32_t pllEnable=1U<<31,pllLock=1U<<30,pllRatioMask=0xff;
constexpr uint32_t pcodeMailbox=0x138124,pcodeData=0x138128,pcodeData1=0x13812c;
constexpr uint32_t pcodeReady=1U<<31,pcodeErrorMask=0xff;
constexpr uint32_t pcodeCdclkControl=0x7,cdclkPrepare=0x3,cdclkReady=0x1;
constexpr uint32_t dbufCtl[2]={0x45008,0x44fe8}; // DBUF_CTL_S(S1), (S2)
constexpr uint32_t dbufRequest=1U<<31,dbufState=1U<<30,dbufTrackerMask=0x1fU<<19;
constexpr uint32_t mbusAbox[3]={0x45038,0x45048,0x4504c}; // MBUS_ABOX_CTL(0-2)
constexpr uint32_t bwBuddyCtl[2]={0x45140,0x45150};       // BW_BUDDY_CTL(1),(2)
constexpr uint32_t bwBuddyDisable=1U<<31;
constexpr uint32_t chickenDcpr2=0x46434;     // GEN11_CHICKEN_DCPR_2
constexpr uint32_t dcprWa=0xfU<<24;          // CLEAR_MEMSTAT_DIS..MAXLATENCY_MEMUP_CLR
// Combo PHY A-E bases (_ICL_COMBOPHY) and offsets (intel_combo_phy_regs.h).
constexpr uint32_t phyBase[5]={0x162000,0x6c000,0x160000,0x161000,0x16b000};
constexpr uint32_t clDw5=0x14,compDw0=0x100,compDw1=0x104,compDw3=0x10c,
 compDw8=0x120,compDw9=0x124,compDw10=0x128,pcsDw1Grp=0x604,pcsDw1Ln0=0x804,
 txDw8Grp=0x6a0,txDw8Ln0=0x8a0;
constexpr uint32_t phyMiscA=0x64c00,deIoCompPwrDown=1U<<23; // ICL_PHY_MISC(A)
constexpr uint32_t compInit=1U<<31,irefgen=1U<<24,clPowerDownEnable=1U<<4;
constexpr uint32_t odccClkSel=1U<<31,odccDivMask=3U<<29,odccDiv2=1U<<29;
constexpr uint32_t dccModeMask=3U<<20;       // RUN_DCC_ONCE = 0
constexpr uint32_t procmonMask=7U<<26|3U<<24;
}
// icl_procmon_values: {dw3 voltage|process, dw1, dw9, dw10}.
struct Procmon {uint32_t key,dw1,dw9,dw10;};
static constexpr Procmon procmon[]={
 {0U<<24|0U<<26,0x00000000,0x62AB67BB,0x51914F96}, // 0.85V dot0
 {1U<<24|0U<<26,0x00000000,0x86E172C7,0x77CA5EAB}, // 0.95V dot0
 {1U<<24|1U<<26,0x00000000,0x93F87FE1,0x8AE871C5}, // 0.95V dot1
 {2U<<24|0U<<26,0x00000000,0x98FA82DD,0x89E46DC1}, // 1.05V dot0
 {2U<<24|1U<<26,0x00440000,0x9A00AB25,0x8AE38FF1}, // 1.05V dot1
};
// CDCLK 307.2 MHz from the 38.4 MHz reference (icl_cdclk_table ratio 16,
// VCO 614.4 MHz): CD2X divide 1, no pipe, decimal 612; voltage level 0.
constexpr uint32_t cdclkRatio=16,cdclkCtlValue=0x00380264,cdclkVoltage=0;
constexpr uint32_t cdclkCtlMask=3U<<22|7U<<19|0x7ff;

template<class IO> class CoreInit {
 IO&io;const bool execute;
public:
 static constexpr unsigned capacity=96;
 Entry log[capacity];unsigned count=0;
 CoreInit(IO&i,bool doWrite):io(i),execute(doWrite){}
private:
 bool record(Step s,Action a,uint32_t reg,uint32_t before,uint32_t after,uint8_t index=0){
  if(count>=capacity)return false;
  log[count++]={uint8_t(s),uint8_t(a),index,0,reg,before,after};return true;
 }
 // Poll until (read & mask)==value; true on success.
 bool waitFor(uint32_t reg,uint32_t mask,uint32_t value,uint32_t timeoutUS){
  const uint64_t deadline=io.nowUS()+timeoutUS;
  for(;;){
   if((io.read(reg)&mask)==value)return true;
   if(io.nowUS()>=deadline)return false;
   io.delayUS(10);
  }
 }
 // Read-modify-write; skipped when the register already holds the result.
 Result rmw(Step s,uint32_t reg,uint32_t clear,uint32_t set,uint8_t index=0){
  const uint32_t before=io.read(reg),want=(before&~clear)|set;
  if(want==before)return record(s,Action::Skip,reg,before,before,index)?Result::OK:Result::LogFull;
  if(!execute)return record(s,Action::Plan,reg,before,want,index)?Result::OK:Result::LogFull;
  io.write(reg,want);
  const uint32_t after=io.read(reg);
  if(!record(s,Action::Write,reg,before,after,index))return Result::LogFull;
  return (after&(clear|set))==(want&(clear|set))?Result::OK:Result::Unexpected;
 }
 // Plain write. Group registers (PHY *_GRP) do not read back; readable
 // registers may skip when they already hold the value.
 Result write(Step s,uint32_t reg,uint32_t value,uint8_t index=0,bool skipIfEqual=false){
  const uint32_t before=io.read(reg);
  if(skipIfEqual&&before==value)return record(s,Action::Skip,reg,before,before,index)?Result::OK:Result::LogFull;
  if(!execute)return record(s,Action::Plan,reg,before,value,index)?Result::OK:Result::LogFull;
  io.write(reg,value);
  return record(s,Action::Write,reg,before,io.read(reg),index)?Result::OK:Result::LogFull;
 }
 Result fail(Step s,Result r,uint32_t reg,uint32_t value){
  record(s,Action::Fail,reg,value,value);return r;
 }
 // One pcode mailbox transaction (__snb_pcode_rw): 0 or the error status,
 // ~0U if busy or no completion within timeoutUS.
 uint32_t pcode(uint32_t command,uint32_t&data,uint32_t timeoutUS){
  if(io.read(R::pcodeMailbox)&R::pcodeReady)return ~0U;
  io.write(R::pcodeData,data);io.write(R::pcodeData1,0);
  io.write(R::pcodeMailbox,R::pcodeReady|command);
  if(!waitFor(R::pcodeMailbox,R::pcodeReady,0,timeoutUS))return ~0U;
  data=io.read(R::pcodeData);
  return io.read(R::pcodeMailbox)&R::pcodeErrorMask;
 }
 const Procmon*procmonFor(uint32_t dw3){
  for(const auto&p:procmon)if(p.key==(dw3&R::procmonMask))return &p;
  return nullptr;
 }
 static bool master(unsigned phy){return phy==0||phy==3;} // ADL-S: A->B,C; D->E
 // icl_combo_phy_verify_state for display 12 (PHY_MISC only on PHY A).
 bool phyReady(unsigned phy,const Procmon&p){
  const uint32_t b=R::phyBase[phy];
  if(phy==0&&(io.read(R::phyMiscA)&R::deIoCompPwrDown))return false;
  if(!(io.read(b+R::compDw0)&R::compInit))return false;
  if((io.read(b+R::txDw8Ln0)&(R::odccClkSel|R::odccDivMask))!=(R::odccClkSel|R::odccDiv2))return false;
  if(io.read(b+R::pcsDw1Ln0)&R::dccModeMask)return false;
  if((io.read(b+R::compDw1)&0x00ff00ffU)!=p.dw1||io.read(b+R::compDw9)!=p.dw9||io.read(b+R::compDw10)!=p.dw10)return false;
  if(master(phy)&&!(io.read(b+R::compDw8)&R::irefgen))return false;
  return io.read(b+R::clDw5)&R::clPowerDownEnable;
 }
 // icl_combo_phys_init for one PHY.
 Result comboPhy(unsigned phy){
  const uint32_t b=R::phyBase[phy];const uint8_t i=uint8_t(phy);
  const uint32_t dw3=io.read(b+R::compDw3);
  const Procmon*p=procmonFor(dw3);
  if(!p)return fail(Step::ComboPhy,Result::Unexpected,b+R::compDw3,dw3);
  if(phyReady(phy,*p))return record(Step::ComboPhy,Action::Skip,b+R::compDw0,io.read(b+R::compDw0),io.read(b+R::compDw0),i)?Result::OK:Result::LogFull;
  Result r=Result::OK;
  if(phy==0)r=rmw(Step::ComboPhy,R::phyMiscA,R::deIoCompPwrDown,0,i);
  // TX_DW8 and PCS_DW1 are read from lane 0 and written to the group.
  if(r==Result::OK)r=write(Step::ComboPhy,b+R::txDw8Grp,(io.read(b+R::txDw8Ln0)&~R::odccDivMask)|R::odccClkSel|R::odccDiv2,i);
  if(r==Result::OK)r=write(Step::ComboPhy,b+R::pcsDw1Grp,io.read(b+R::pcsDw1Ln0)&~R::dccModeMask,i);
  if(r==Result::OK)r=rmw(Step::ComboPhy,b+R::compDw1,0x00ff00ffU,p->dw1,i);
  if(r==Result::OK)r=write(Step::ComboPhy,b+R::compDw9,p->dw9,i);
  if(r==Result::OK)r=write(Step::ComboPhy,b+R::compDw10,p->dw10,i);
  if(r==Result::OK&&master(phy))r=rmw(Step::ComboPhy,b+R::compDw8,0,R::irefgen,i);
  if(r==Result::OK)r=rmw(Step::ComboPhy,b+R::compDw0,0,R::compInit,i);
  if(r==Result::OK)r=rmw(Step::ComboPhy,b+R::clDw5,0,R::clPowerDownEnable,i);
  return r;
 }
 Result powerWell1(){
  const uint32_t ctl=io.read(R::pwrWellDriver);
  if((ctl&(R::pw1Request|R::pw1State))==(R::pw1Request|R::pw1State))
   return record(Step::PowerWell1,Action::Skip,R::pwrWellDriver,ctl,ctl)?Result::OK:Result::LogFull;
  Result r=rmw(Step::PowerWell1,R::pwrWellDriver,0,R::pw1Request);
  if(r!=Result::OK||!execute)return r;
  if(!waitFor(R::pwrWellDriver,R::pw1State,R::pw1State,1000))
   return fail(Step::PowerWell1,Result::Timeout,R::pwrWellDriver,io.read(R::pwrWellDriver));
  if(!waitFor(R::fuseStatus,R::fusePG1,R::fusePG1,1000))
   return fail(Step::PowerWell1,Result::Timeout,R::fuseStatus,io.read(R::fuseStatus));
  return Result::OK;
 }
 Result cdclk(){
  const uint32_t pll=io.read(R::cdclkPll),ctl=io.read(R::cdclkCtl);
  if((pll&(R::pllEnable|R::pllLock|R::pllRatioMask))==(R::pllEnable|R::pllLock|cdclkRatio)&&
     (ctl&cdclkCtlMask)==cdclkCtlValue){
   record(Step::CdclkPll,Action::Skip,R::cdclkPll,pll,pll);
   return record(Step::CdclkCtl,Action::Skip,R::cdclkCtl,ctl,ctl)?Result::OK:Result::LogFull;
  }
  if(!execute){
   record(Step::CdclkPrepare,Action::Plan,R::pcodeMailbox,R::pcodeCdclkControl,R::cdclkPrepare);
   if(pll&R::pllEnable)record(Step::CdclkPll,Action::Plan,R::cdclkPll,pll,pll&~R::pllEnable,1);
   record(Step::CdclkPll,Action::Plan,R::cdclkPll,pll,R::pllEnable|cdclkRatio);
   record(Step::CdclkCtl,Action::Plan,R::cdclkCtl,ctl,cdclkCtlValue);
   return record(Step::CdclkVoltage,Action::Plan,R::pcodeMailbox,R::pcodeCdclkControl,cdclkVoltage)?Result::Planned:Result::LogFull;
  }
  // skl_pcode_request: resend until ready, 3 ms then up to 50 ms more.
  bool ready=false;uint32_t status=0,data=0;
  for(const uint64_t deadline=io.nowUS()+53000;!ready&&io.nowUS()<deadline;){
   data=R::cdclkPrepare;status=pcode(R::pcodeCdclkControl,data,500);
   if(status!=~0U&&status!=0)break;
   ready=status==0&&(data&R::cdclkReady)==R::cdclkReady;
   if(!ready)io.delayUS(10);
  }
  record(Step::CdclkPrepare,ready?Action::Write:Action::Fail,R::pcodeMailbox,status,data);
  if(!ready)return status==~0U?Result::Timeout:Result::Unexpected;
  // icl_cdclk_pll_update: disable a PLL at another ratio, then enable.
  if(pll&R::pllEnable){
   io.write(R::cdclkPll,pll&~R::pllEnable);
   const bool unlocked=waitFor(R::cdclkPll,R::pllLock,0,1000);
   record(Step::CdclkPll,unlocked?Action::Write:Action::Fail,R::cdclkPll,pll,io.read(R::cdclkPll),1);
   if(!unlocked)return Result::Timeout;
  }
  io.write(R::cdclkPll,cdclkRatio);
  io.write(R::cdclkPll,cdclkRatio|R::pllEnable);
  const bool locked=waitFor(R::cdclkPll,R::pllLock,R::pllLock,1000);
  record(Step::CdclkPll,locked?Action::Write:Action::Fail,R::cdclkPll,pll,io.read(R::cdclkPll));
  if(!locked)return Result::Timeout;
  io.write(R::cdclkCtl,cdclkCtlValue);
  record(Step::CdclkCtl,Action::Write,R::cdclkCtl,ctl,io.read(R::cdclkCtl));
  // snb_pcode_write with a 1 ms timeout.
  data=cdclkVoltage;status=pcode(R::pcodeCdclkControl,data,1000);
  record(Step::CdclkVoltage,status==0?Action::Write:Action::Fail,R::pcodeMailbox,status,cdclkVoltage);
  return status==0?Result::OK:status==~0U?Result::Timeout:Result::Unexpected;
 }
 Result dbuf(){
  Result r=Result::OK;
  // gen12_dbuf_slices_config: tracker state service 8 on both slices.
  for(uint8_t s=0;s<2&&r==Result::OK;++s)r=rmw(Step::DbufTracker,R::dbufCtl[s],R::dbufTrackerMask,8U<<19,s);
  if(r!=Result::OK)return r;
  // gen9_dbuf_enable: power at least slice S1, keep slices already on.
  r=rmw(Step::DbufPower,R::dbufCtl[0],0,R::dbufRequest);
  if(r!=Result::OK||!execute)return r;
  io.delayUS(10);
  const uint32_t v=io.read(R::dbufCtl[0]);
  return v&R::dbufState?Result::OK:fail(Step::DbufPower,Result::Timeout,R::dbufCtl[0],v);
 }
public:
 Result run(){
  // Preconditions: 38.4 MHz reference, PG0 fuses distributed, DC states off.
  const uint32_t dssm=io.read(R::dssm),fuse=io.read(R::fuseStatus),dc=io.read(R::dcStateEn);
  record(Step::Preconditions,Action::Check,R::dssm,dssm,dssm);
  if((dssm>>29)!=2)return fail(Step::Preconditions,Result::PreconditionFailed,R::dssm,dssm);
  if(!(fuse&R::fusePG0))return fail(Step::Preconditions,Result::PreconditionFailed,R::fuseStatus,fuse);
  // gen9_set_dc_state(DC_STATE_DISABLE) is not ported: DC states must be off.
  if(dc&R::dcStateMask)return fail(Step::DCOff,Result::PreconditionFailed,R::dcStateEn,dc);
  record(Step::DCOff,Action::Skip,R::dcStateEn,dc,dc);
  Result r=rmw(Step::WaPchClockGate,R::southClockGate,0,R::dpmgunitGate); // Wa_14011294188
  if(r==Result::OK)r=rmw(Step::PchHandshake,R::rstwrnOpt,0,R::pchHandshake);
  for(unsigned phy=0;phy<5&&r==Result::OK;++phy)r=comboPhy(phy);
  if(r==Result::OK)r=powerWell1();
  if(r==Result::OK){r=cdclk();if(r==Result::Planned)r=Result::OK;}
  if(r==Result::OK)r=dbuf();
  // icl_mbus_init: display 12 programs ABOX0 as well as ABOX1/2.
  for(uint8_t a=0;a<3&&r==Result::OK;++a)
   r=rmw(Step::Mbus,R::mbusAbox[a],0x3U<<20|0xfU<<16|0x1fU<<8|0x1fU,1U<<20|1U<<16|16U<<8|16U,a);
  // tgl_bw_buddy_init: DRAM type is unknown in the guest, so take i915's
  // "unknown memory configuration" path and disable the buddy logic.
  for(uint8_t a=0;a<2&&r==Result::OK;++a)r=write(Step::BwBuddy,R::bwBuddyCtl[a],R::bwBuddyDisable,uint8_t(a+1),true);
  if(r==Result::OK)r=rmw(Step::WaDcpr,R::chickenDcpr2,0,R::dcprWa); // Wa_14011508470
  if(r!=Result::OK)return r;
  record(Step::Done,Action::Check,0,0,0);
  return execute?Result::OK:Result::Planned;
 }
};
}
