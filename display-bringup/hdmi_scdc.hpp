#pragma once
#include "edid_read.hpp"

// HDMI 2.0 SCDC setup over the PCH GMBUS controller, for pixel clocks above
// 340 MHz (e.g. 3840x2160@60, 594 MHz). Ported from Linux i915 v7.2
// intel_hdmi_handle_sink_scrambling with the drm SCDC helpers
// (drm_scdc_set_high_tmds_clock_ratio, drm_scdc_set_scrambling,
// drm_scdc_get_scrambling_status). SCDC lives at I2C slave 0x54 on the same
// DDC pin as the EDID; this writes the sink's TMDS_CONFIG register (control
// I2C to the monitor, not the display pipe). Reuses the GMBUS register layout
// and wait from edid_read.hpp.
namespace ReimsSCDC {
namespace G=ReimsEDID::R;
constexpr uint8_t kSlave=0x54;
constexpr uint8_t kTmdsConfig=0x20,kScramblerStatus=0x21;
constexpr uint8_t kScramblingEnable=1<<0,kBitClockRatioBy40=1<<1,kScramblingStatus=1<<0;
enum class Status:uint32_t {OK,Busy,Nak,Timeout};

template<class IO> class Scdc {
 IO&io;
 bool wait(uint32_t status,uint32_t timeoutUS){
  const uint64_t deadline=io.nowUS()+timeoutUS;
  for(;;){
   const uint32_t g2=io.read(G::gmbus2);
   if(g2&G::satoer)return false;
   if(g2&status)return true;
   if(io.nowUS()>=deadline)return false;
   io.delayUS(5);
  }
 }
 Status finish(Status s){io.write(G::gmbus1,G::cycleStop|G::swRdy);io.write(G::gmbus0,0);return s;}
 Status begin(uint8_t pin){
  if(io.read(G::gmbus2)&G::inUse)return Status::Busy;
  io.write(G::gmbus0,G::rate100|pin);io.write(G::gmbus4,0);
  return Status::OK;
 }
public:
 explicit Scdc(IO&i):io(i){}
 // Combined index(offset) + 1-byte read from slave 0x54.
 Status readb(uint8_t pin,uint8_t offset,uint8_t&val){
  const Status b=begin(pin);if(b!=Status::OK)return b;
  io.write(G::gmbus1,G::swRdy|G::cycleIndex|(uint32_t(offset)<<G::slaveIndexShift)|G::cycleWait|
           (1U<<G::byteCountShift)|(uint32_t(kSlave)<<G::slaveAddrShift)|G::slaveRead);
  if(!wait(G::hwRdy,50000))return finish(io.read(G::gmbus2)&G::satoer?Status::Nak:Status::Timeout);
  val=io.read(G::gmbus3)&0xff;
  return finish(Status::OK);
 }
 // 2-byte write [offset, value] to slave 0x54.
 Status writeb(uint8_t pin,uint8_t offset,uint8_t val){
  const Status b=begin(pin);if(b!=Status::OK)return b;
  io.write(G::gmbus3,uint32_t(offset)|uint32_t(val)<<8);
  io.write(G::gmbus1,G::swRdy|G::cycleWait|(2U<<G::byteCountShift)|
           (uint32_t(kSlave)<<G::slaveAddrShift)); // GMBUS_SLAVE_WRITE = 0
  if(!wait(G::hwRdy,50000))return finish(io.read(G::gmbus2)&G::satoer?Status::Nak:Status::Timeout);
  return finish(Status::OK);
 }
 // One TMDS_CONFIG read-modify-write (drm_scdc_set_* each do one).
 Status rmwTmdsConfig(uint8_t pin,uint8_t setBits,uint8_t clearBits){
  uint8_t cfg=0;const Status r=readb(pin,kTmdsConfig,cfg);if(r!=Status::OK)return r;
  cfg=uint8_t((cfg&~clearBits)|setBits);
  return writeb(pin,kTmdsConfig,cfg);
 }
 // intel_hdmi_handle_sink_scrambling: high ratio then scrambling (i915 order).
 Status setup(uint8_t pin,bool highRatio,bool scrambling){
  Status r=rmwTmdsConfig(pin,highRatio?kBitClockRatioBy40:0,highRatio?0:kBitClockRatioBy40);
  if(r!=Status::OK)return r;
  return rmwTmdsConfig(pin,scrambling?kScramblingEnable:0,scrambling?0:kScramblingEnable);
 }
 bool scramblingStatus(uint8_t pin){
  uint8_t s=0;return readb(pin,kScramblerStatus,s)==Status::OK&&(s&kScramblingStatus);
 }
};
}
