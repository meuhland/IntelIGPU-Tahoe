#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../display-bringup/gt_sampler.hpp"

// Low-GGTT scrub for the hold build (2026-10-16, opt-in REIMS_GT_GGTT_SCRUB).
//
// Apple's initGlobalGTT adopts the GGTT the host i915 left behind instead of
// resetting unused entries to a scratch page the way i915 does. Before the
// first context load the low GGTT therefore still holds i915's addresses:
// entry 0 read 0x4ff44b000 (the 2026-10-16 window), ~20 GiB, outside this
// 16 GiB guest's RAM. A GPU access that resolves through such an entry leaves
// the VM's address space, which the host IOMMU cannot attribute to the guest
// (DMAR "context entry not present"); that is the write-to-0 / CAT_ERROR
// signature. i915 never leaves a GGTT entry pointing at memory it does not
// own, so this makes the low GGTT follow that rule before igpu-start: every
// present entry below Apple's 1 GiB allocation window that points outside
// guest RAM (or at physical 0) is redirected to one scratch page we own, so a
// stray access lands harmlessly instead of faulting. Apple reprograms what it
// needs from 1 GiB up, so nothing it uses is touched. Reads/writes GGTT PTEs
// only (BAR0 upper half, no forcewake); no GT register is written.
namespace ReimsGTScrub {

// Apple's own GGTT allocations begin at 1 GiB (status page 0x40004000, LRCA
// 0x402b7000, ... on 2026-10-16), so only entries below this are host stale.
constexpr uint32_t windowBytes = 0x40000000u;            // 1 GiB of address space
constexpr unsigned windowPages = windowBytes / 4096u;    // 262144 GGTT entries

// Guest-physical layout of VM 113: q35, 16 GiB, no balloon. RAM sits below the
// PCI hole at 0x8000_0000 (2 GiB) and resumes at 0x1_0000_0000, 14 GiB more,
// to 0x4_8000_0000. An address in the hole or at/above the top is not guest
// RAM. Change ramTop with the VM's memory size (top = 0x1_0000_0000 + size-2G).
constexpr uint64_t pciHoleLo = 0x80000000ull;
constexpr uint64_t pciHoleHi = 0x100000000ull;
constexpr uint64_t guestRamTop16G = 0x480000000ull;

// Gen12 GGTT PTE: bit 0 present, bits 38:12 physical address.
constexpr uint64_t pteAddrMask = 0x7ffffff000ull;
inline uint64_t ptePhysical(uint64_t pte) { return pte & pteAddrMask; }
inline uint64_t scratchPte(uint64_t scratchPhys) { return (scratchPhys & pteAddrMask) | 1ull; }

inline bool inGuestRam(uint64_t phys, uint64_t ramTop) {
 if (phys < pciHoleLo) return true;
 return phys >= pciHoleHi && phys < ramTop;
}

// A present entry is a bad target when it points at physical 0 or anywhere
// that is not guest RAM; those are the only entries that fault or corrupt.
// Absent entries (bit 0 clear) are left as they are.
inline bool badTarget(uint64_t pte, uint64_t ramTop) {
 if (!(pte & 1)) return false;
 const uint64_t phys = ptePhysical(pte);
 return phys == 0 || !inGuestRam(phys, ramTop);
}

struct Counts { unsigned present = 0, redirected = 0, scanned = 0; };

// Redirect every bad low-GGTT entry to the scratch page. IO is BarIO-like
// (uint32_t read(uint32_t); void write(uint32_t,uint32_t)); ggttBase is the
// GGTT's BAR0 offset (ReimsGTSampler::ggttBase). Stops early if the window
// would run past the mapped BAR. Returns the counts.
template <class IO>
inline Counts scrub(IO& io, uint64_t barLength, uint64_t scratchPhys, uint64_t ramTop,
                    unsigned pages = windowPages, uint32_t ggttBase = ReimsGTSampler::ggttBase) {
 Counts c;
 const uint64_t spte = scratchPte(scratchPhys);
 for (unsigned p = 0; p < pages; ++p) {
  const uint64_t off = uint64_t(ggttBase) + uint64_t(p) * 8;
  if (off + 8 > barLength) break;
  ++c.scanned;
  const uint64_t pte = uint64_t(io.read(uint32_t(off + 4))) << 32 | io.read(uint32_t(off));
  if (pte & 1) ++c.present;
  if (badTarget(pte, ramTop)) {
   io.write(uint32_t(off), uint32_t(spte));
   io.write(uint32_t(off + 4), uint32_t(spte >> 32));
   ++c.redirected;
  }
 }
 return c;
}
}
