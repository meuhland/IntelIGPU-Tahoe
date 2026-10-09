#pragma once
#include <stdint.h>

// Read-only EDID read over the PCH GMBUS controller and a minimal block-0
// parser, ported from Linux i915 v7.2 intel_gmbus.c (gmbus_xfer_read_chunk /
// gmbus_index_xfer / do_gmbus_xfer) for display 12 with an ADP PCH. The GMBUS
// registers live in the PCH window at 0xc0000 (PCH_DISPLAY_BASE); the base
// EDID is 128 bytes at I2C slave 0x50, offset 0. This drives the I2C engine
// (writes to GMBUS control registers) but reads only the monitor; it never
// touches the display pipe. The DDC pin comes from the VBT (HDMI-B = pin 2).
//
// IO provides uint32_t read(uint32_t), void write(uint32_t,uint32_t),
// void delayUS(uint32_t), uint64_t nowUS().
namespace ReimsEDID {
namespace R {
constexpr uint32_t base=0xc0000;
constexpr uint32_t gmbus0=base+0x5100,gmbus1=base+0x5104,gmbus2=base+0x5108,
 gmbus3=base+0x510c,gmbus4=base+0x5110,gmbus5=base+0x5120;
constexpr uint32_t swClrInt=1U<<31,swRdy=1U<<30,cycleWait=1U<<25,cycleIndex=2U<<25,
 cycleStop=4U<<25,byteCountShift=16,slaveIndexShift=8,slaveAddrShift=1,slaveRead=1U;
constexpr uint32_t hwRdy=1U<<11,satoer=1U<<10,active=1U<<9,inUse=1U<<15;
constexpr uint32_t rate100=0;
}
enum class Status:uint32_t {OK,Busy,Nak,Timeout,BadHeader,BadChecksum};

struct Edid {
 uint8_t raw[128]={};
 char vendor[4]={};         // 3-letter PnP manufacturer ID
 uint16_t product=0;uint32_t serial=0;
 uint8_t week=0;uint16_t year=0;
 uint8_t version=0,revision=0;
 bool digital=false;uint8_t extensions=0;
 uint32_t prefPixelClockKHz=0,prefHActive=0,prefVActive=0;
};

template<class IO> class Reader {
 IO&io;
 bool wait(uint32_t status,uint32_t timeoutUS){
  const uint64_t deadline=io.nowUS()+timeoutUS;
  for(;;){
   const uint32_t g2=io.read(R::gmbus2);
   if(g2&R::satoer)return false;        // NAK: caller maps to Nak
   if(g2&status)return true;
   if(io.nowUS()>=deadline)return false;
   io.delayUS(5);
  }
 }
public:
 explicit Reader(IO&i):io(i){}
 // Read 128-byte block 0 from slave 0x50 at offset 0 on the given DDC pin.
 Status readBlock0(uint8_t pin,uint8_t out[128]){
  if(io.read(R::gmbus2)&R::inUse)return Status::Busy;
  io.write(R::gmbus0,R::rate100|pin);
  io.write(R::gmbus4,0);
  // Combined index(offset 0) + read of 128 bytes (gmbus_index_xfer path).
  io.write(R::gmbus1,R::swRdy|R::cycleIndex|(0U<<R::slaveIndexShift)|R::cycleWait|
           (128U<<R::byteCountShift)|(0x50U<<R::slaveAddrShift)|R::slaveRead);
  unsigned got=0;
  while(got<128){
   if(!wait(R::hwRdy,50000)){
    const bool nak=io.read(R::gmbus2)&R::satoer;
    io.write(R::gmbus1,R::cycleStop|R::swRdy);io.write(R::gmbus0,0);
    return nak?Status::Nak:Status::Timeout;
   }
   uint32_t val=io.read(R::gmbus3);
   for(unsigned i=0;i<4&&got<128;++i){out[got++]=val&0xff;val>>=8;}
  }
  // STOP, wait for the bus to go idle, release.
  io.write(R::gmbus1,R::cycleStop|R::swRdy);
  const uint64_t deadline=io.nowUS()+10000;
  while((io.read(R::gmbus2)&R::active)&&io.nowUS()<deadline)io.delayUS(5);
  io.write(R::gmbus0,0);
  return Status::OK;
 }
 Status read(uint8_t pin,Edid&e){
  const Status s=readBlock0(pin,e.raw);
  if(s!=Status::OK)return s;
  return parse(e.raw,e);
 }
 static Status parse(const uint8_t raw[128],Edid&e){
  static const uint8_t header[8]={0,0xff,0xff,0xff,0xff,0xff,0xff,0};
  for(unsigned i=0;i<8;++i)if(raw[i]!=header[i])return Status::BadHeader;
  uint8_t sum=0;for(unsigned i=0;i<128;++i)sum=uint8_t(sum+raw[i]);
  if(sum)return Status::BadChecksum;
  if(e.raw!=raw)for(unsigned i=0;i<128;++i)e.raw[i]=raw[i];
  const uint16_t id=uint16_t(raw[8]<<8|raw[9]);
  e.vendor[0]=char(((id>>10)&0x1f)+'A'-1);
  e.vendor[1]=char(((id>>5)&0x1f)+'A'-1);
  e.vendor[2]=char((id&0x1f)+'A'-1);
  e.vendor[3]=0;
  e.product=uint16_t(raw[10]|raw[11]<<8);
  e.serial=uint32_t(raw[12]|raw[13]<<8|raw[14]<<16|raw[15]<<24);
  e.week=raw[16];e.year=uint16_t(1990+raw[17]);
  e.version=raw[18];e.revision=raw[19];
  e.digital=(raw[20]&0x80)!=0;
  e.extensions=raw[126];
  // First detailed timing descriptor at offset 54.
  const uint8_t*d=raw+54;
  e.prefPixelClockKHz=uint32_t(d[0]|d[1]<<8)*10;
  e.prefHActive=uint32_t(d[2]|((d[4]&0xf0)<<4));
  e.prefVActive=uint32_t(d[5]|((d[7]&0xf0)<<4));
  return Status::OK;
 }
};
}
