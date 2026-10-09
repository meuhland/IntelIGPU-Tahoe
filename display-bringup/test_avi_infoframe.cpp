// Host-side test of avi_infoframe.hpp. Anchor: the host's captured 4K60 AVI
// DIP dwords (igpu-reference-20261009-145010, transcoder A 0x60220/24/28:
// 0x000d0282 / 0x042812d0 / 0x00000061) must be reproduced exactly from
// VIC 97, limited range. Then check the port's own 4K30 full-range packet
// and the checksum invariant.
#include <stdio.h>
#include "avi_infoframe.hpp"

using namespace ReimsAVI;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

// Sum of header + 13 data bytes + checksum == 0 mod 256 (hole excluded).
static bool checksumValid(const Packet&p){
 uint8_t bytes[18];
 bytes[0]=p.dw[0]&0xff;bytes[1]=p.dw[0]>>8&0xff;bytes[2]=p.dw[0]>>16&0xff;bytes[3]=p.dw[0]>>24&0xff;
 for(int i=1;i<8;++i){bytes[2+i*2]=p.dw[i]&0xff;} // not used; explicit sum below
 uint8_t sum=uint8_t((p.dw[0]&0xff)+(p.dw[0]>>8&0xff)+(p.dw[0]>>16&0xff)); // header
 const uint8_t cksum=p.dw[1]&0xff;
 const uint8_t d[13]={uint8_t(p.dw[1]>>8),uint8_t(p.dw[1]>>16),uint8_t(p.dw[1]>>24),
  uint8_t(p.dw[2]),uint8_t(p.dw[2]>>8),uint8_t(p.dw[2]>>16),uint8_t(p.dw[2]>>24),
  uint8_t(p.dw[3]),uint8_t(p.dw[3]>>8),uint8_t(p.dw[3]>>16),uint8_t(p.dw[3]>>24),
  uint8_t(p.dw[4]),uint8_t(p.dw[4]>>8)};
 for(uint8_t b:d)sum=uint8_t(sum+b);
 (void)bytes;
 return uint8_t(sum+cksum)==0;
}

int main(){
 // Anchor: reproduce the host's live 4K60 limited-range AVI packet.
 auto host=build(97,Range::Limited);
 CHECK(host.dw[0]==0x000d0282);
 CHECK(host.dw[1]==0x042812d0);
 CHECK(host.dw[2]==0x00000061);
 CHECK(host.dw[3]==0&&host.dw[4]==0);
 CHECK(checksumValid(host));
 // The port's 4K30 full-range packet: VIC 95, Q=full (2).
 auto ours=build(vicFor2160p(30),Range::Full);
 CHECK(vicFor2160p(30)==95);
 CHECK((ours.dw[2]&0xff)==95);                 // data byte 4 = VIC
 CHECK(((ours.dw[1]>>24)&0x0c)==0x08);          // data byte 3 Q bits = full
 CHECK(checksumValid(ours));
 // Full range differs from the host's limited only in VIC and the Q field
 // (and therefore the checksum); byte 1/2 unchanged.
 CHECK((ours.dw[1]>>8&0xff)==0x12&&(ours.dw[1]>>16&0xff)==0x28);
 // Every 2160p rate produces a valid checksum and the right VIC.
 const uint32_t rates[]={24,25,30,50,60};const uint8_t vics[]={93,94,95,96,97};
 for(int i=0;i<5;++i){auto p=build(vicFor2160p(rates[i]),Range::Full);
  CHECK(vicFor2160p(rates[i])==vics[i]&&(p.dw[2]&0xff)==vics[i]&&checksumValid(p));}
 if(failures){printf("%d failure(s)\n",failures);return 1;}
 printf("PASS: AVI infoframe (host 4K60 packet reproduced, 4K30 full-range, checksums)\n");
 return 0;
}
