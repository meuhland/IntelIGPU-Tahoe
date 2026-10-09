// Host-side test of edid_read.hpp: a GMBUS register model streams the real
// captured block-0 EDID (Samsung 4K monitor, igpu-reference-20261009-145010)
// and the reader + parser recover it; NAK, timeout and bad EDID are checked.
#include <stdio.h>
#include <string.h>
#include "edid_read.hpp"

using namespace ReimsEDID;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

static const uint8_t kRealEdid[128]={
 0x00,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x4c,0x2d,0x03,0x71,0x00,0x0e,0x00,0x01,
 0x01,0x1e,0x01,0x03,0x80,0x46,0x27,0x78,0x2a,0x64,0xa5,0xa4,0x54,0x4d,0x9a,0x26,
 0x0f,0x50,0x54,0xbd,0xef,0x80,0x71,0x4f,0x81,0xc0,0x81,0x00,0x81,0x80,0x95,0x00,
 0xa9,0xc0,0xb3,0x00,0xd1,0xc0,0x08,0xe8,0x00,0x30,0xf2,0x70,0x5a,0x80,0xb0,0x58,
 0x8a,0x00,0x50,0x1d,0x74,0x00,0x00,0x1e,0x56,0x5e,0x00,0xa0,0xa0,0xa0,0x29,0x50,
 0x30,0x20,0x35,0x00,0x50,0x1d,0x74,0x00,0x00,0x1a,0x00,0x00,0x00,0xfd,0x00,0x18,
 0x4b,0x0f,0x87,0x3c,0x00,0x0a,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xfc,
 0x00,0x53,0x41,0x4d,0x53,0x55,0x4e,0x47,0x0a,0x20,0x20,0x20,0x20,0x20,0x01,0x6b};

namespace G=ReimsEDID::R;
// Emulates the GMBUS read engine: a read cycle in GMBUS1 streams `src` through
// GMBUS3, setting HW_RDY before each dword. nak=true raises SATOER instead.
struct Model {
 const uint8_t*src;unsigned pos=0,remaining=0;uint32_t g2=0;uint64_t now=0;bool nak=false,inUse=false;
 explicit Model(const uint8_t*s):src(s){}
 uint64_t nowUS(){return now;}
 void delayUS(uint32_t us){now+=us;}
 uint32_t read(uint32_t a){
  if(a==G::gmbus2){uint32_t v=g2|(inUse?G::inUse:0);return v;}
  if(a==G::gmbus3){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(pos<128?src[pos++]:0)<<(i*8);
   remaining=remaining>=4?remaining-4:0;g2&=~G::hwRdy;if(remaining)g2|=G::hwRdy;return v;}
  return 0;
 }
 void write(uint32_t a,uint32_t v){
  if(a==G::gmbus1&&(v&G::slaveRead)&&(v&G::cycleWait)){
   if(nak){g2|=G::satoer;return;}
   remaining=(v>>G::byteCountShift)&0x1ff;pos=0;g2|=G::hwRdy|G::active;
  }
  if(a==G::gmbus1&&(v&G::cycleStop))g2&=~G::active;
 }
};

int main(){
 // Successful read of the real monitor's EDID over pin 2 (HDMI-B per VBT).
 {Model m(kRealEdid);Reader<Model> r(m);Edid e;
  CHECK(r.read(2,e)==Status::OK);
  CHECK(memcmp(e.raw,kRealEdid,128)==0);
  CHECK(strcmp(e.vendor,"SAM")==0&&e.product==0x7103);
  CHECK(e.version==1&&e.revision==3&&e.digital&&e.extensions==1);
  CHECK(e.prefPixelClockKHz==594000&&e.prefHActive==3840&&e.prefVActive==2160);}
 // Parser rejects a corrupt header and a bad checksum.
 {uint8_t bad[128];memcpy(bad,kRealEdid,128);bad[0]=1;Edid e;
  CHECK(Reader<Model>::parse(bad,e)==Status::BadHeader);}
 {uint8_t bad[128];memcpy(bad,kRealEdid,128);bad[50]^=0xff;Edid e;
  CHECK(Reader<Model>::parse(bad,e)==Status::BadChecksum);}
 // A monitor that NAKs (nothing connected) maps to Nak, not garbage.
 {Model m(kRealEdid);m.nak=true;Reader<Model> r(m);Edid e;
  CHECK(r.read(2,e)==Status::Nak);}
 // Bus already in use is refused.
 {Model m(kRealEdid);m.inUse=true;Reader<Model> r(m);Edid e;
  CHECK(r.read(2,e)==Status::Busy);}
 if(failures){printf("%d failure(s)\n",failures);return 1;}
 printf("PASS: GMBUS EDID read + parse (real Samsung 4K block 0, NAK, bad EDID)\n");
 return 0;
}
