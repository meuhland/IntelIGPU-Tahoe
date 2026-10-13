#pragma once
#include <stdint.h>

// RPL-S handoff of the pipe the port's bring-up lit (display-bringup stage 3):
// plane 1A on pipe/transcoder A, linear XRGB8888, full screen at the origin,
// scanning a GGTT range the CPU reaches through BAR2 (GMADR). The stub
// IONDRVFramebuffer on the IGD drives nothing, so the mode comes from this
// hardware state instead of the boot framebuffer. Read-only; no MMIO writes.
// Display 12 plane registers (intel_display_regs.h, skl_universal_plane.c).
namespace ReimsTakeover {
constexpr uint32_t planeCtl=0x70180,planeStride=0x70188,planePos=0x7018c,planeSize=0x70190,
                   planeSurf=0x7019c,planeOffset=0x701a4,planeSurfLive=0x701ac;
struct Plane {uint32_t ctl=0,stride=0,pos=0,size=0,surf=0,offset=0,surfLive=0;};
enum class Status:uint32_t {OK,PlaneOff,Tiled,Format,Origin,Geometry,Stride,Surface,NotLive,Aperture};
struct Result {
 Status status=Status::PlaneOff;
 uint32_t width=0,height=0,pitchBytes=0,surface=0;
 uint64_t length=0;
};
// PLANE_CTL: enable bit 31, tiling 12:10, format 27:23 (XRGB8888 = 4 << 24),
// RGBX order bit 20 clear. PLANE_STRIDE is in 64-byte units for linear.
inline Result check(const Plane&p,uint64_t apertureLength){
 Result r;
 if(!(p.ctl&0x80000000U))return r;
 if(p.ctl&(7U<<10)){r.status=Status::Tiled;return r;}
 if((p.ctl&0x0f900000U)!=0x04000000U){r.status=Status::Format;return r;}
 if(p.pos||p.offset){r.status=Status::Origin;return r;}
 r.width=(p.size&0xffffU)+1;r.height=(p.size>>16)+1;
 if(r.width<640||r.height<480||r.width>8192||r.height>8192){r.status=Status::Geometry;return r;}
 const uint32_t units=p.stride&0xfffU;
 r.pitchBytes=units*64;
 if(p.stride!=units||r.pitchBytes<r.width*4){r.status=Status::Stride;return r;}
 // 4 KiB GGTT address, no flag bits (host i915 scanned linear from
 // 0x00aa4000; the bring-up uses 0x01000000).
 r.surface=p.surf&0xfffff000U;
 if(!r.surface||p.surf!=r.surface){r.status=Status::Surface;return r;}
 if((p.surfLive&0xfffff000U)!=r.surface){r.status=Status::NotLive;return r;}
 r.length=uint64_t(r.pitchBytes)*r.height;
 if(r.surface+r.length>apertureLength){r.status=Status::Aperture;return r;}
 r.status=Status::OK;return r;
}
// The bring-up publishes only the 128-byte base EDID block (ReimsEDIDBlock0).
// Serve it to IOGraphics as a complete EDID: validate the header and checksum,
// clear the extension count (byte 126) so no missing CTA block is requested,
// and recompute the checksum (byte 127). False leaves out untouched.
inline bool edidBaseOnly(const uint8_t in[128],uint8_t out[128]){
 static const uint8_t header[8]={0,0xff,0xff,0xff,0xff,0xff,0xff,0};
 uint8_t sum=0;
 for(unsigned i=0;i<128;++i){if(i<8&&in[i]!=header[i])return false;sum=uint8_t(sum+in[i]);}
 if(sum)return false;
 sum=0;
 for(unsigned i=0;i<127;++i){out[i]=i==126?0:in[i];sum=uint8_t(sum+out[i]);}
 out[127]=uint8_t(0x100-sum);
 return true;
}
}
