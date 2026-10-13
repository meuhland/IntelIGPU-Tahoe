// Host test of the shared framebuffer fill (fb_memory.hpp), the pixels the
// bring-up and the option-C self-light path scan out. KERNEL is undefined, so
// the IOKit allocator is excluded and only the fills compile.
#define REIMS_FB_HOST_TEST 1  // exclude the IOKit allocator; test the fills only
#include <cstdio>
#include <cstdint>
#include <vector>
#include "fb_memory.hpp"

using namespace ReimsBringup;
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;}}while(0)

int main(){
 std::vector<uint32_t> buf(3840u * 2160u, 0xDEADBEEF);

 fillBlack(buf.data());
 bool allZero = true;
 for (uint32_t v : buf) allZero &= v == 0;
 CHECK(allZero);

 fillTestPattern(buf.data());
 auto at = [&](uint32_t x, uint32_t y) { return buf[y * 3840u + x]; };
 // 16-pixel white border.
 CHECK(at(0, 0) == 0xffffff && at(3839, 0) == 0xffffff);
 CHECK(at(0, 2159) == 0xffffff && at(3839, 2159) == 0xffffff);
 CHECK(at(15, 500) == 0xffffff && at(3824, 500) == 0xffffff);
 // First colour bar inside the border is white (bars[0]); a later column differs.
 CHECK(at(20, 800) == 0xffffff);
 CHECK(at(2000, 800) != at(20, 800));
 // Grey ramp in the bottom quarter: grey (r==g==b), rising left to right.
 uint32_t g1 = at(100, 1700), g2 = at(3000, 1700);
 auto grey = [](uint32_t c) { return (c & 0xff) == ((c >> 8) & 0xff) && (c & 0xff) == ((c >> 16) & 0xff); };
 CHECK(grey(g1) && grey(g2) && (g2 & 0xff) > (g1 & 0xff));
 // No stray value left from the pre-fill.
 bool clean = true;
 for (uint32_t v : buf) clean &= v != 0xDEADBEEF;
 CHECK(clean);

 if (failures) return 1;
 puts("PASS: framebuffer fill (black zeroes, test-pattern border/bars/ramp)");
 return 0;
}
