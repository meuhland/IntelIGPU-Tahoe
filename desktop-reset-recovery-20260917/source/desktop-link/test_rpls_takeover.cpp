// Host-side test of the RPL-S takeover plane check (rpls_takeover.hpp) and the
// mode-derived layout helpers (native_layout.hpp). Plane values are the ones the
// bring-up programs (display-bringup/hdmi_scanout.hpp) and host i915's capture.
#include <stdio.h>
#include "rpls_takeover.hpp"
#include "native_layout.hpp"

using namespace ReimsTakeover;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

static constexpr uint64_t kGMADR=256ULL<<20;
static Plane bringup(){
 Plane p;p.ctl=0x84000000;p.stride=0xf0;p.size=0x086f0eff;p.surf=0x01000000;p.surfLive=0x01000000;
 return p;
}

int main(){
 // Bring-up stage 3b: 3840x2160 linear XRGB8888 at GGTT 16 MiB.
 auto r=check(bringup(),kGMADR);
 CHECK(r.status==Status::OK);
 CHECK(r.width==3840&&r.height==2160&&r.pitchBytes==15360&&r.surface==0x01000000);
 CHECK(r.length==15360ULL*2160);
 // Host i915's own linear fbdev plane (16 KiB aligned surface).
 auto host=bringup();host.surf=host.surfLive=0x00aa4000;
 CHECK(check(host,kGMADR).status==Status::OK);
 // SURFLIVE may carry low bits; only the address is compared.
 auto live=bringup();live.surfLive|=0x3;
 CHECK(check(live,kGMADR).status==Status::OK);

 auto p=bringup();p.ctl&=~0x80000000U;CHECK(check(p,kGMADR).status==Status::PlaneOff);
 p=bringup();p.ctl|=1U<<10;CHECK(check(p,kGMADR).status==Status::Tiled);
 p=bringup();p.ctl=0x88000000;CHECK(check(p,kGMADR).status==Status::Format);
 p=bringup();p.ctl|=1U<<20;CHECK(check(p,kGMADR).status==Status::Format);
 p=bringup();p.pos=0x00010001;CHECK(check(p,kGMADR).status==Status::Origin);
 p=bringup();p.offset=0x40;CHECK(check(p,kGMADR).status==Status::Origin);
 p=bringup();p.size=0x00ef013f;CHECK(check(p,kGMADR).status==Status::Geometry);
 p=bringup();p.stride=0xe0;CHECK(check(p,kGMADR).status==Status::Stride);
 p=bringup();p.stride=0x10f0;CHECK(check(p,kGMADR).status==Status::Stride);
 p=bringup();p.surf=0;p.surfLive=0;CHECK(check(p,kGMADR).status==Status::Surface);
 p=bringup();p.surf|=0x4;CHECK(check(p,kGMADR).status==Status::Surface);
 p=bringup();p.surfLive=0x02000000;CHECK(check(p,kGMADR).status==Status::NotLive);
 CHECK(check(bringup(),0x01000000+15360ULL*2160-1).status==Status::Aperture);
 CHECK(check(bringup(),0x01000000+15360ULL*2160).status==Status::OK);

 // Float bits replace the hard-coded full-screen rectangle.
 using ReimsNativeLayout::floatBits;
 static_assert(floatBits(1920)==0x44f00000U&&floatBits(1080)==0x44870000U,"ADL-P rect");
 static_assert(floatBits(3840)==0x45700000U&&floatBits(2160)==0x45070000U,"RPL-S rect");
 static_assert(floatBits(1)==0x3f800000U&&floatBits(0)==0,"edges");

 // Layout plan for a linear 4K BGRA surface at the mode size.
 ReimsNativeLayout::Input in;
 in.width=3840;in.height=2160;in.surfacePitch=15360;in.fourcc=0x42475241;in.planeCount=1;
 in.tileMode=0;in.resourceFormat=0xc0;in.rowA=15360;in.backingLength=15360ULL*2160;in.storageKnown=true;
 const auto plan=ReimsNativeLayout::describe(in,3840,2160);
 CHECK(plan.status==ReimsNativeLayout::Status::OK&&plan.planeStride==0xf0&&plan.requiredBytes==15360ULL*2160);
 CHECK(ReimsNativeLayout::describe(in).status==ReimsNativeLayout::Status::BadGeometry);
 in.width=1920;in.height=1080;in.surfacePitch=in.rowA=7680;in.backingLength=7680ULL*1080;
 CHECK(ReimsNativeLayout::describe(in).status==ReimsNativeLayout::Status::OK);

 if(failures)return 1;
 puts("test_rpls_takeover: OK");return 0;
}
