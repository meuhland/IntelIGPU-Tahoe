#pragma once
#include <stdint.h>

// Ventura TGL 16.0.0 kIntelTileMode -> ADL-P display layout. No MMIO writes.
// Native enum: 0=linear, 2=X, 3=legacy Y. The display tiling encoding differs.
// Evidence and scope: RENDER-TARGET-LAYOUT-2026-09-11.md.
namespace ReimsNativeLayout {
enum class Status : uint32_t {
 OK, BadGeometry, UnsupportedFormat, UnsupportedTileMode, UnsupportedPlanes,
 PitchMismatch, PitchAlignment, StrideOverflow, OffsetUnsupported,
 BackingTooShort, CompressionUnsupported, StorageUnknown
};
struct Input {
 uint64_t width=0,height=0,surfacePitch=0;
 uint32_t fourcc=0,planeCount=0;
 uint32_t tileMode=0,resourceFormat=0;
 uint64_t rowA=0,rowB=0,clientOffset=0,planeOffset=0,backingLength=0;
 bool storageKnown=false,compressed=false;
};
struct Plan {
 Status status=Status::BadGeometry;
 uint32_t tileWidthBytes=0,tileHeight=0,storageHeight=0,pitchBytes=0;
 uint32_t planeTilingBits=0,planeStride=0,baseAlignment=0;
 uint64_t requiredBytes=0;
};
// IEEE-754 single-precision bits of a positive integer below 2^24, without
// floating point (kernel code): 1920 -> 0x44f00000, 3840 -> 0x45700000.
constexpr uint32_t floatBits(uint32_t v){
 return v?((127U+31U-uint32_t(__builtin_clz(v)))<<23)|((v<<(23U-(31U-uint32_t(__builtin_clz(v)))))&0x7fffffU):0;
}
inline Plan describe(const Input&i,uint32_t modeWidth=1920,uint32_t modeHeight=1080){
 Plan p;
 // Current framebuffer mode. Cropping, rotation and planar/CCS need separate
 // contracts; a successful layout plan does not approve a complete flip.
 if(i.width!=modeWidth||i.height!=modeHeight)return p;
 if(i.fourcc!=0x42475241||i.resourceFormat!=0xc0){p.status=Status::UnsupportedFormat;return p;}
 if(i.planeCount>1){p.status=Status::UnsupportedPlanes;return p;}
 switch(i.tileMode){
 case 0:p.tileWidthBytes=64;p.tileHeight=1;p.baseAlignment=256*1024;break;
 case 2:p.tileWidthBytes=512;p.tileHeight=8;p.planeTilingBits=1U<<10;p.baseAlignment=256*1024;break;
 case 3:p.tileWidthBytes=128;p.tileHeight=32;p.planeTilingBits=4U<<10;p.baseAlignment=1024*1024;break;
 default:p.status=Status::UnsupportedTileMode;return p;
 }
 // This is exactly the native flip's b8 / c0 selection. +188 is NOT a
 // universally byte-valued pitch and must not be used as PLANE_STRIDE.
 const uint64_t row=i.rowA?i.rowA:i.rowB;
 if(row!=i.surfacePitch||row<uint64_t(i.width)*4||row>UINT32_MAX){p.status=Status::PitchMismatch;return p;}
 p.pitchBytes=uint32_t(row);
 if(row%p.tileWidthBytes){p.status=Status::PitchAlignment;return p;}
 p.planeStride=p.pitchBytes/p.tileWidthBytes;
 if(!p.planeStride||p.planeStride>0xfff){p.status=Status::StrideOverflow;return p;}
 // Only base-backed surfaces until subrange mapping offsets are recovered.
 if(i.clientOffset||i.planeOffset){p.status=Status::OffsetUnsupported;return p;}
 // Geometry was checked against the mode above, before this narrowing.
 p.storageHeight=uint32_t((i.height+p.tileHeight-1)/p.tileHeight*p.tileHeight);
 p.requiredBytes=uint64_t(p.pitchBytes)*p.storageHeight;
 if(p.requiredBytes>i.backingLength){p.status=Status::BackingTooShort;return p;}
 if(!i.storageKnown){p.status=Status::StorageUnknown;return p;}
 if(i.compressed){p.status=Status::CompressionUnsupported;return p;}
 p.status=Status::OK;return p;
}
// For a later GGTT scanout only. Mapping identity and render/latch completion
// are independent requirements; metadata validation must not assert them.
inline bool baseSuitable(const Plan&p,uint64_t va){
 return p.status==Status::OK&&p.baseAlignment&&!(va%p.baseAlignment)&&
        va<=UINT32_MAX&&p.requiredBytes<=0x100000000ULL-va;
}
}
