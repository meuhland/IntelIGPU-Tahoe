#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../display-bringup/gt_sampler.hpp"

// Context-image dump for the hold build (2026-10-16). RCS hits a catastrophic
// error on its first context load: CONTEXT_CONTROL is restored, the ring
// registers stay 0, and the GPU issues a DMA write to address 0. The
// descriptor Apple submits sits in RING_EXECLIST_SQ_CONTENTS (rcsSq0Lo, LRCA
// in bits 31:12). For each new LRCA the sampler thread reads the GGTT PTEs of
// the context's first pages, maps those guest-physical pages read-only, and
// prints their first dwords: page 0 is the per-process status page, page 1
// the register state the engine restores (an MI_LOAD_REGISTER_IMM list,
// starting with CONTEXT_CONTROL and the ring registers, then the PPGTT root).
// Memory reads only; no GPU register is written.
namespace ReimsCtxDump {

constexpr unsigned pages=2;          // PPHWSP and register state
constexpr unsigned dwords=96;        // per page: covers R_PWR_CLK_STATE (Gen12 RCS ~dword 67)
constexpr unsigned perLine=8;        // 8 x 9 + prefix stays far under 255
constexpr unsigned ringPage=2;       // printed as "p2": the ring buffer (from RING_START)
// 2026-10-16 dump: Apple's own context and ring were sane and mapped into
// guest RAM, so the next suspects are what that context pulls in. Labels:
constexpr unsigned poshPage=3;       // "p3": POSH context page 1 (POSH_LRCA, LRI 0x1b0)
constexpr unsigned rootPage=4;       // "p4": PPGTT root (PDP0, a physical page, not GGTT)
constexpr unsigned sq1Page=5;        // "p5": second submitted context (SQ1) page 1
constexpr uint32_t lriPoshLrca=0x1b0,lriPdp0Ldw=0x270,lriPdp0Udw=0x274;
// Apple's GGTT allocations start at 1 GiB (status page at 0x40004000 on
// 2026-10-16). The first build listed 1 MiB of PTEs (formatGtt, still read
// by the decoder); the probe now runs the anomaly scan below instead.
constexpr uint32_t gttWindow=0x40000000;
constexpr unsigned gttPages=256;
constexpr unsigned gttPerLine=6;     // 6 x 17 + prefix stays far under 255
// Anomaly scan: Apple's objects reached 0x402d7000 by the first submission,
// past the 1 MiB window. Scan 8 MiB and print only entries that are not
// valid or valid with physical 0, capped, plus one summary line.
constexpr unsigned scanPages=2048;
constexpr unsigned scanMaxLines=48;
struct ScanSummary {unsigned valid=0,invalid=0,zero=0,printed=0;};
// true when this entry should be printed (invalid or physical 0)
inline bool scanAnomaly(uint64_t pte,ScanSummary&s){
 if(!(pte&1)){++s.invalid;return true;}
 ++s.valid;
 if(!(pte&0x7ffffff000ull)){++s.zero;return true;}
 return false;
}

// Value of the register with engine-relative offset rel (or its RCS absolute
// 0x2000+rel) in the MI_LOAD_REGISTER_IMM blocks of a context state page.
inline bool findLri(const uint32_t*w,unsigned n,uint32_t rel,uint32_t&value){
 for(unsigned i=0;i<n;){
  if((w[i]>>23)==0x22){
   const unsigned pairs=((w[i]&0xff)+1)/2;
   for(unsigned k=0;k<pairs&&i+2+2*k<n;++k){
    const uint32_t reg=w[i+1+2*k]&0xfffff;
    if(reg==rel||reg==0x2000+rel){value=w[i+2+2*k];return true;}
   }
   i+=1+2*pairs;
  } else ++i;
 }
 return false;
}
constexpr uint32_t lriRingStart=0x038;

inline uint32_t lrcaOf(uint32_t sq0Lo){return sq0Lo&0xfffff000u;}

// GGTT PTE offset in BAR0 of the given page of the context.
inline uint64_t pteOffset(uint32_t lrca,unsigned page){
 return uint64_t(ReimsGTSampler::ggttBase)+(uint64_t(lrca>>12)+page)*8;
}
inline bool pteValid(uint64_t pte){return pte&1;}
inline uint64_t ptePhysical(uint64_t pte){return pte&0x7ffffff000ull;}

// Dump once per distinct, nonzero LRCA.
struct Tracker {
 uint32_t last=0;
 bool shouldDump(uint32_t sq0Lo){
  const uint32_t l=lrcaOf(sq0Lo);
  if(!l||l==last)return false;
  last=l;return true;
 }
};

// "ReimsGT ctx <lrca> p<page> pte <16 hex>" (the page header line).
inline size_t formatPte(char*out,size_t n,uint32_t lrca,unsigned page,uint64_t pte){
 static const char hex[]="0123456789abcdef";
 size_t len=0;
 auto put=[&](char c){if(len+1<n)out[len]=c;++len;};
 auto str=[&](const char*p){while(*p)put(*p++);};
 auto hx=[&](uint64_t v,int digits){for(int b=(digits-1)*4;b>=0;b-=4)put(hex[(v>>b)&0xf]);};
 str("ReimsGT ctx ");hx(lrca,8);str(" p");put(char('0'+page));str(" pte ");hx(pte,16);put('\n');
 if(n)out[len<n?len:n-1]='\0';
 return len;
}
// "ReimsGT ctx <lrca> p<page> @<first> v0 v1 ..." (perLine dwords).
inline size_t formatWords(char*out,size_t n,uint32_t lrca,unsigned page,unsigned first,const uint32_t*v,unsigned count){
 static const char hex[]="0123456789abcdef";
 size_t len=0;
 auto put=[&](char c){if(len+1<n)out[len]=c;++len;};
 auto str=[&](const char*p){while(*p)put(*p++);};
 auto hx=[&](uint64_t x,int digits){for(int b=(digits-1)*4;b>=0;b-=4)put(hex[(x>>b)&0xf]);};
 auto dec=[&](unsigned x){char d[10];unsigned k=0;do{d[k++]=char('0'+x%10);x/=10;}while(x);while(k)put(d[--k]);};
 str("ReimsGT ctx ");hx(lrca,8);str(" p");put(char('0'+page));str(" @");dec(first);
 for(unsigned i=0;i<count;++i){put(' ');hx(v[i],8);}
 put('\n');
 if(n)out[len<n?len:n-1]='\0';
 return len;
}
// "ReimsGT gttbad <gtt> <pte>" and "ReimsGT gttscan <start> <pages> valid <v> invalid <i> zero <z>".
inline size_t formatScanEntry(char*out,size_t n,uint32_t gtt,uint64_t pte){
 static const char hex[]="0123456789abcdef";
 size_t len=0;
 auto put=[&](char c){if(len+1<n)out[len]=c;++len;};
 auto str=[&](const char*p){while(*p)put(*p++);};
 auto hx=[&](uint64_t x,int digits){for(int b=(digits-1)*4;b>=0;b-=4)put(hex[(x>>b)&0xf]);};
 str("ReimsGT gttbad ");hx(gtt,8);put(' ');hx(pte,16);put('\n');
 if(n)out[len<n?len:n-1]='\0';
 return len;
}
inline size_t formatScanSummary(char*out,size_t n,uint32_t start,unsigned pages,const ScanSummary&s){
 static const char hex[]="0123456789abcdef";
 size_t len=0;
 auto put=[&](char c){if(len+1<n)out[len]=c;++len;};
 auto str=[&](const char*p){while(*p)put(*p++);};
 auto hx=[&](uint64_t x,int digits){for(int b=(digits-1)*4;b>=0;b-=4)put(hex[(x>>b)&0xf]);};
 auto dec=[&](unsigned x){char d[10];unsigned k=0;do{d[k++]=char('0'+x%10);x/=10;}while(x);while(k)put(d[--k]);};
 str("ReimsGT gttscan ");hx(start,8);put(' ');dec(pages);str(" valid ");dec(s.valid);
 str(" invalid ");dec(s.invalid);str(" zero ");dec(s.zero);put('\n');
 if(n)out[len<n?len:n-1]='\0';
 return len;
}
// "ReimsGT gtt <gtt address of the first entry> pte pte ..." (16 hex each).
inline size_t formatGtt(char*out,size_t n,uint32_t gtt,const uint64_t*pte,unsigned count){
 static const char hex[]="0123456789abcdef";
 size_t len=0;
 auto put=[&](char c){if(len+1<n)out[len]=c;++len;};
 auto str=[&](const char*p){while(*p)put(*p++);};
 auto hx=[&](uint64_t x,int digits){for(int b=(digits-1)*4;b>=0;b-=4)put(hex[(x>>b)&0xf]);};
 str("ReimsGT gtt ");hx(gtt,8);
 for(unsigned i=0;i<count;++i){put(' ');hx(pte[i],16);}
 put('\n');
 if(n)out[len<n?len:n-1]='\0';
 return len;
}
}
