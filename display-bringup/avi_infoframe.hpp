#pragma once
#include <stdint.h>

// CEA-861 AVI InfoFrame (version 2, 13 data bytes) packed for the HSW+ video
// DIP data registers, as Linux i915 v7.2 does it (intel_hdmi.c
// intel_write_infoframe / hsw_write_infoframe): the 3-byte header, a zero
// "hole" byte, the checksum, then the 13 data bytes, written little-endian as
// dwords into HSW_TVIDEO_DIP_AVI_DATA. The checksum covers the real infoframe
// (header + 13 data bytes), not the hole.
//
// Only the fields this port's fixed RGB HDMI output needs are set: pixel
// encoding RGB, 16:9, the video code (VIC) and the RGB quantization range.
// Anchored by test against the host's captured 4K60 packet.
namespace ReimsAVI {
enum class Range:uint8_t {Default=0,Limited=1,Full=2}; // CEA Q field

// data byte 1 (Y/A/B/S): RGB (Y=0), active-format-present, scan 2 ("overscan"
// as the host sends); data byte 2 (C/M/R): no-data colorimetry, 16:9 (M=2),
// same-as-coded aspect (R=8). These match the host capture.
constexpr uint8_t kByte1=0x12,kByte2=0x28;

struct Packet {uint32_t dw[8]={};};

inline Packet build(uint8_t vic,Range range){
 uint8_t d[13]={};
 d[0]=kByte1;
 d[1]=kByte2;
 d[2]=uint8_t((uint8_t(range)&3)<<2); // data byte 3: Q at bits 3:2
 d[3]=vic;                             // data byte 4: VIC
 // d[4..12] = 0 (pixel repetition 0, no bar info).
 uint8_t sum=0x82+0x02+0x0d;
 for(uint8_t b:d)sum=uint8_t(sum+b);
 const uint8_t checksum=uint8_t(0x100-sum);
 Packet p;
 p.dw[0]=0x82u|0x02u<<8|0x0du<<16;                       // type, version, length, hole=0
 p.dw[1]=uint32_t(checksum)|uint32_t(d[0])<<8|uint32_t(d[1])<<16|uint32_t(d[2])<<24;
 p.dw[2]=uint32_t(d[3])|uint32_t(d[4])<<8|uint32_t(d[5])<<16|uint32_t(d[6])<<24;
 p.dw[3]=uint32_t(d[7])|uint32_t(d[8])<<8|uint32_t(d[9])<<16|uint32_t(d[10])<<24;
 p.dw[4]=uint32_t(d[11])|uint32_t(d[12])<<8;
 return p;
}

// CEA VICs for 3840x2160 RGB: 24/25/30/50/60 Hz.
constexpr uint8_t vicFor2160p(uint32_t refreshHz){
 return refreshHz==24?93:refreshHz==25?94:refreshHz==30?95:refreshHz==50?96:refreshHz==60?97:0;
}
}
