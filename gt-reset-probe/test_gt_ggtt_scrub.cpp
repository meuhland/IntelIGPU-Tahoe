// Host test of the low-GGTT scrub helpers (gt_ggtt_scrub.hpp).
#include <stdio.h>
#include <map>
#include "gt_ggtt_scrub.hpp"

using namespace ReimsGTScrub;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

// A fake BAR0: 32-bit cells keyed by offset, default 0.
struct FakeBar {
 std::map<uint32_t, uint32_t> cell;
 unsigned reads = 0, writes = 0;
 uint32_t read(uint32_t o) { ++reads; auto it = cell.find(o); return it == cell.end() ? 0 : it->second; }
 void write(uint32_t o, uint32_t v) { ++writes; cell[o] = v; }
 void setPte(unsigned page, uint64_t pte) {
  const uint32_t off = ReimsGTSampler::ggttBase + page * 8;
  cell[off] = uint32_t(pte); cell[off + 4] = uint32_t(pte >> 32);
 }
 uint64_t getPte(unsigned page) {
  const uint32_t off = ReimsGTSampler::ggttBase + page * 8;
  return uint64_t(cell[off + 4]) << 32 | cell[off];
 }
};

int main() {
 const uint64_t top = guestRamTop16G;

 // Classification: guest RAM below the hole and in the high bank is kept;
 // the PCI hole, at/above the top, and physical 0 are redirected.
 CHECK(inGuestRam(0x1000, top) && inGuestRam(0x7ffff000, top));
 CHECK(!inGuestRam(0x80000000ull, top) && !inGuestRam(0xfffff000ull, top));  // PCI hole
 CHECK(inGuestRam(0x100000000ull, top) && inGuestRam(0x47ffff000ull, top));  // high bank
 CHECK(!inGuestRam(0x480000000ull, top) && !inGuestRam(0x4ff44b000ull, top));  // at/above top
 CHECK(!badTarget(0x14b757000ull, top));                 // present, guest RAM: keep
 CHECK(badTarget(0x4ff44b00bull, top));                  // present, 20 GiB: the 2026-10-16 stale entry
 CHECK(badTarget(0x8bull, top));                          // present, physical 0
 CHECK(badTarget(0x80000001ull, top));                    // present, PCI hole
 CHECK(!badTarget(0x4ff44b000ull, top));                  // bit 0 clear (not present): leave it
 CHECK(scratchPte(0x135000000ull) == 0x135000001ull);

 // A fake low GGTT: a stale host entry, a hole entry, a physical-0 entry, a
 // real guest-RAM entry, and an absent entry. Only the three bad present ones
 // are redirected to the scratch page; the real one and the absent one stay.
 FakeBar bar;
 const uint64_t scratch = 0x135abc000ull;
 bar.setPte(0, 0x4ff44b00bull);        // stale host, ~20 GiB
 bar.setPte(1, 0x0000008bull);          // physical 0
 bar.setPte(2, 0x14b757003ull);         // guest RAM, present
 bar.setPte(3, 0x90000001ull);          // PCI hole
 bar.setPte(4, 0x14b758000ull);         // guest RAM, not present (bit 0 clear)
 const unsigned pages = 8;
 Counts c = scrub(bar, uint64_t(ReimsGTSampler::ggttBase) + pages * 8, scratch, top, pages);
 CHECK(c.scanned == pages && c.present == 4 && c.redirected == 3);
 CHECK(bar.getPte(0) == scratchPte(scratch));
 CHECK(bar.getPte(1) == scratchPte(scratch));
 CHECK(bar.getPte(2) == 0x14b757003ull);   // untouched guest-RAM mapping
 CHECK(bar.getPte(3) == scratchPte(scratch));
 CHECK(bar.getPte(4) == 0x14b758000ull);   // untouched absent entry
 CHECK(bar.getPte(5) == 0 && bar.getPte(6) == 0 && bar.getPte(7) == 0);  // empty stay empty

 // The window stops at the mapped BAR length, never reading past it.
 FakeBar small;
 small.setPte(0, 0x4ff44b00bull);
 Counts cs = scrub(small, uint64_t(ReimsGTSampler::ggttBase) + 8, scratch, top, 100);
 CHECK(cs.scanned == 1 && cs.redirected == 1);

 // Apple's window and page count are consistent (1 GiB at 4 KiB).
 CHECK(windowPages == 262144 && windowBytes == windowPages * 4096);

 if (failures) return 1;
 puts("PASS: GGTT scrub (guest-RAM classification, redirect only bad present entries, BAR bound)");
 return 0;
}
