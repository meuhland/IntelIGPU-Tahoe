#pragma once
#include <stdint.h>
#include <stddef.h>

// Read-only GT watch for the phase-6 accelerator start. The first start on
// a780 (2026-10-13) wedged IOKit: ioreg and logd blocked, so neither registry
// properties nor the unified log could report where the GPU init stopped.
// The sampler (a timer in the bring-up kext, threads in the gt-reset-probe
// hold build) prints these registers with IOLog: besides os_log, IOLog writes
// to the console, which serial=3 puts on the serial port, without logd.
// kprintf does NOT: on a release kernel it only reaches serial with the
// debug=0x8 (DB_KPRT) boot-arg, otherwise it goes to os_log alone (xnu
// pe_kprintf.c), which is why the 2026-10-15 watch printed nothing.
//
// Reads only. It never requests forcewake, so GT-domain registers read as 0
// while the GT sleeps; the forcewake acks in the same sample show which. The
// GGTT entries live in BAR0's upper half and need no forcewake.
// Offsets: i915 intel_gt_regs.h, intel_engine_regs.h (RENDER 0x2000,
// BLT 0x22000), gt/uc/intel_guc_reg.h; display ones as in hdmi_scanout.hpp.
namespace ReimsGTSampler {
// derive: 0 = read offset; 1/2 = low/high dword of the GGTT PTE that the
// already-sampled RCS status-page address (rcsHwsPga) points at; 3/4 = the
// same for the context address (LRCA) in rcsSq0Lo.
struct Reg {uint32_t offset;const char*name;uint8_t derive=0;};
constexpr uint32_t ggttBase=0x800000;  // BAR0 upper half, as hdmi_scanout.hpp
constexpr Reg regs[]={
 {0x130044,"fwAckGT"},{0x000d84,"fwAckRender"},
 {0x00c000,"gucStatus"},{0x00c180,"gucScratch0"},{0x00c314,"gucDmaCtrl"},
 {0x002034,"rcsHead"},{0x002030,"rcsTail"},{0x002038,"rcsStart"},{0x00203c,"rcsCtl"},
 {0x002074,"rcsActhd"},{0x002068,"rcsIpehr"},{0x002234,"rcsExecStatus"},{0x0023a0,"rcsCsb"},
 {0x0020d0,"rcsReset"},{0x00209c,"rcsMiMode"},
 {0x022034,"bcsHead"},{0x022030,"bcsTail"},{0x02203c,"bcsCtl"},{0x022074,"bcsActhd"},
 {0x0040a0,"errorGen6"},{0x00cec4,"ringFault"},
 // Interrupt path (2026-10-15: is a GT interrupt raised and left unserviced
 // while start waits?). Plain reads; i915 clears these only by writing back.
 // i915_reg.h GEN11_GFX_MSTR_IRQ; intel_gt_regs.h GEN11_GT_INTR_DW(0/1),
 // GEN11_RENDER_COPY_INTR_ENABLE; intel_engine_regs.h RING_IMR/EIR/ESR/HWS_PGA.
 {0x190010,"gfxMstrIrq"},{0x190018,"gtIntrDw0"},{0x19001c,"gtIntrDw1"},
 {0x190030,"rcsBcsIntrEn"},{0x0020a8,"rcsImr"},{0x0020b0,"rcsEir"},
 {0x0020b8,"rcsEsr"},{0x002080,"rcsHwsPga"},
 // Error state of a stalled engine (2026-10-15 window: RCS RING_RESET_CTL
 // showed CAT_ERROR and no context ever loaded). intel_engine_regs.h
 // RING_IPEIR/INSTDONE/DMA_FADD/BBADDR/EXECLIST_STATUS_HI/CONTEXT_CONTROL;
 // intel_gt_regs.h GEN12_FAULT_TLB_DATA0/1 (with GEN12_RING_FAULT_REG above).
 {0x002064,"rcsIpeir"},{0x00206c,"rcsInstdone"},{0x002078,"rcsDmaFadd"},
 {0x002140,"rcsBbaddr"},{0x002238,"rcsExecStatusHi"},{0x002244,"rcsCtxCtl"},
 {0x00ceb8,"faultTlb0"},{0x00cebc,"faultTlb1"},
 // Where the GPU writes (2026-10-15: host DMAR logged a GPU DMA write to
 // address 0x0). The status page's GGTT PTE (derived from rcsHwsPga above)
 // and GGTT entry 0; a valid PTE with address 0 sends writes to physical 0.
 {0,"hwsPteLo",1},{0,"hwsPteHi",2},
 {ggttBase,"ggttPte0Lo"},{ggttBase+4,"ggttPte0Hi"},
 // The submitted context (2026-10-16: CAT_ERROR on context load, ring regs
 // never restored, CONTEXT_CONTROL first). Apple's
 // IGHardwareCommandStreamer5::submitExecList writes two 64-bit element
 // descriptors to kIGHwCsDesc+0x19 and 1 to +0x1d: for RCS 0x2510 and
 // 0x2550, i915 RING_EXECLIST_SQ_CONTENTS / RING_EXECLIST_CONTROL. Element 0
 // low dword = LRCA (bits 31:12) | flags. lrcaPte: the GGTT PTE of that LRCA
 // (derived, like hwsPte). GFX_MODE (0x229c, RING_MODE_GEN7) stays: Apple's
 // initModeRegisters clears GEN11_GFX_DISABLE_LEGACY_MODE there, as on ADL-P.
 // (RCS_ELST0/1, PDP0 and STATUS_CTXT_ST_PTR, sampled once, read 0 on Gen12.)
 {0x002510,"rcsSq0Lo"},{0x002514,"rcsSq0Hi"},{0x002518,"rcsSq1Lo"},{0x00251c,"rcsSq1Hi"},
 {0x00229c,"rcsGfxMode"},
 // Engine-side view and power state (2026-10-16). CCID (i915 intel_engine_regs.h
 // CCID(base)+0x180): the context the engine considers current. RC6/power
 // gating (intel_gt_regs.h): Apple never writes RC_CONTROL and writes
 // PG_ENABLE only in the GuC scheduler, so these should stay as the reset
 // left them; RC6_LOCATION/RC6_CTX_BASE are the BIOS-set power-context base.
 {0x002180,"rcsCcid"},{0x00a090,"rcControl"},{0x00a094,"rcState"},{0x00a210,"pgEnable"},
 {0x000d40,"rc6Location"},{0x000d48,"rc6CtxBase"},
 {0,"lrcaPteLo",3},{0,"lrcaPteHi",4},
 // POSH and CCS (2026-10-16 static trace). Apple builds every Metal graphics
 // context with a POSH context for the POCS (base 0x18000, TGL PRM vol 8) and,
 // with EnableMultiContext, keeps the CCS (0x1a000) and enables it in
 // GEN12_RCU_MODE (0x14800, i915 intel_gt_regs.h). Neither engine is used by
 // i915 on Gen12 LP. Their reset/execlist state shows whether either is the
 // engine that faults at the first submission.
 {0x0180d0,"pocsReset"},{0x018234,"pocsExecStatus"},{0x01809c,"pocsMiMode"},
 {0x018080,"pocsHwsPga"},{0x018244,"pocsCtxCtl"},
 {0x01a0d0,"ccsReset"},{0x01a234,"ccsExecStatus"},{0x014800,"rcuMode"},
 {0x070008,"transconfA"},{0x0701ac,"surfLiveA"},
 // GGTT PTE of the bring-up's first framebuffer page (GGTT 0x01000000):
 // ggttBase 0x800000 + (0x01000000 >> 12) * 8, as hdmi_scanout.hpp writes it.
 // A change during PrepareRuntime means the accelerator reused the range.
 {0x808000,"fbPteLo"},{0x808004,"fbPteHi"},
};
constexpr unsigned count=sizeof(regs)/sizeof(regs[0]);
constexpr uint32_t maxOffset(){uint32_t m=0;for(const auto&r:regs)if(!r.derive&&r.offset>m)m=r.offset;return m;}
constexpr int indexOf(const char*name){
 for(unsigned i=0;i<count;++i){
  const char*a=regs[i].name;const char*b=name;
  while(*a&&*a==*b){++a;++b;}
  if(*a==*b)return int(i);
 }
 return -1;
}
constexpr int hwsIndex=indexOf("rcsHwsPga");
constexpr int sq0Index=indexOf("rcsSq0Lo");
constexpr int sq1Index=indexOf("rcsSq1Lo");
// Derived reads that would fall outside the mapped BAR read as this marker.
constexpr uint32_t outsideBar=0xffffffffu;

// Read column i. Plain columns read their offset; derived ones use the
// GGTT address already read into soFar (rcsHwsPga or rcsSq0Lo, which
// precede them in regs[]).
template<class Read> inline uint32_t readColumn(unsigned i,const uint32_t*soFar,uint64_t mapLength,Read&&read){
 const Reg&r=regs[i];
 if(!r.derive)return read(r.offset);
 const uint32_t gtt=soFar[r.derive<=2?hwsIndex:sq0Index];
 const uint64_t off=uint64_t(ggttBase)+uint64_t(gtt>>12)*8+((r.derive&1)?0:4);
 return off+4<=mapLength?read(uint32_t(off)):outsideBar;
}

struct Sample {uint32_t v[count]={};};
// Kext line buffer for format()/header(); the test checks the worst case fits.
constexpr size_t lineBuffer=1024;
// IOLog's console path keeps at most 255 characters of a line
// (SERIAL_CONS_BUF_SIZE 256, xnu osfmk/console/serial_protos.h; the rest is
// dropped unless drain_uart_sync=1), which cut every 2026-10-15 sample after
// 26 columns. The kexts therefore print a sample as parts of partColumns
// registers, "ReimsGT <seq> <ms> @<first> v...", each well under 255.
constexpr unsigned partColumns=10;
constexpr unsigned parts=(count+partColumns-1)/partColumns;
constexpr size_t consoleLine=255;

// Print policy: every sample that differs from the last printed one, plus a
// heartbeat (an unchanged GT then reads as "frozen", not as "sampler dead"),
// up to a line budget so a busy start cannot flood the console.
struct Policy {
 uint32_t heartbeatTicks=20;  // 5 s at 250 ms
 uint32_t maxLines=4000;
};
struct State {
 Sample last;bool havePrinted=false;uint32_t sinceHeartbeat=0,lines=0;
 bool shouldPrint(const Sample&s,const Policy&p){
  if(lines>=p.maxLines)return false;
  bool changed=!havePrinted;
  for(unsigned i=0;i<count&&!changed;++i)changed=s.v[i]!=last.v[i];
  if(!changed&&++sinceHeartbeat<p.heartbeatTicks)return false;
  last=s;havePrinted=true;sinceHeartbeat=0;++lines;return true;
 }
};

// "ReimsGT <seq> <ms> v0 v1 ...": fixed register order, 8 hex digits each.
// The register names are printed once with header(). Returns the length.
// part >= 0 prints only columns [part*partColumns, +partColumns) behind an
// "@<first column>" marker; part < 0 prints every column on one line.
inline unsigned partFirst(int part){return part<0?0:unsigned(part)*partColumns;}
inline unsigned partEnd(int part){
 if(part<0)return count;
 const unsigned e=partFirst(part)+partColumns;
 return e<count?e:count;
}
inline size_t format(char*out,size_t n,uint32_t seq,uint64_t ms,const Sample&s,int part=-1){
 static const char hex[]="0123456789abcdef";
 size_t len=0;
 auto put=[&](char c){if(len+1<n)out[len]=c;++len;};
 auto dec=[&](uint64_t v){char d[20];unsigned k=0;do{d[k++]=char('0'+v%10);v/=10;}while(v);while(k)put(d[--k]);};
 for(const char*p="ReimsGT ";*p;++p)put(*p);
 dec(seq);put(' ');dec(ms);
 if(part>=0){put(' ');put('@');dec(partFirst(part));}
 for(unsigned i=partFirst(part);i<partEnd(part);++i){put(' ');for(int b=28;b>=0;b-=4)put(hex[(s.v[i]>>b)&0xf]);}
 put('\n');
 if(n)out[len<n?len:n-1]='\0';
 return len;
}
inline size_t header(char*out,size_t n,int part=-1){
 size_t len=0;
 auto put=[&](char c){if(len+1<n)out[len]=c;++len;};
 auto dec=[&](uint64_t v){char d[20];unsigned k=0;do{d[k++]=char('0'+v%10);v/=10;}while(v);while(k)put(d[--k]);};
 for(const char*p="ReimsGT seq ms";*p;++p)put(*p);
 if(part>=0){put(' ');put('@');dec(partFirst(part));}
 for(unsigned i=partFirst(part);i<partEnd(part);++i){put(' ');for(const char*p=regs[i].name;*p;++p)put(*p);}
 put('\n');
 if(n)out[len<n?len:n-1]='\0';
 return len;
}
}
