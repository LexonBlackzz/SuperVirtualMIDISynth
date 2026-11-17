// ASIO host tests: legal buffer sizes and the owner-thread host driven by a
// fake IASIO driver. Runs everywhere ASIO is built; needs no audio hardware.
#include <cstdio>
#include <cstdint>
#include <vector>
#include "driver/AsioSizes.h"

using namespace svms;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

using Sizes = std::vector<uint32_t>;

static void TestLegalSizes() {
    CHECK(AsioLegalSizes(8, 2048, 32, -1) == (Sizes{8, 16, 32, 64, 128, 256, 512, 1024, 2048}));
    CHECK(AsioLegalSizes(256, 256, 256, 0) == (Sizes{256}));
    CHECK(AsioLegalSizes(64, 512, 128, 0) == (Sizes{64, 512}));
    CHECK(AsioLegalSizes(96, 480, 96, 96) == (Sizes{96, 192, 288, 384, 480}));
    CHECK(AsioLegalSizes(48, 2048, 48, -1) == (Sizes{48, 64, 128, 256, 512, 1024, 2048}));
    CHECK(AsioLegalSizes(64, 1000, 64, -1) == (Sizes{64, 128, 256, 512, 1000}));
    CHECK(AsioLegalSizes(0, 0, 0, 0).empty());
    CHECK(AsioLegalSizes(512, 256, 256, -1).empty());
}

static void TestNearestLegalSize() {
    const Sizes p2{8, 16, 32, 64, 128, 256, 512, 1024, 2048};
    CHECK(AsioNearestLegalSize(p2, 32) == 32);
    CHECK(AsioNearestLegalSize(p2, 24) == 32);   // tie 16/32 -> larger
    CHECK(AsioNearestLegalSize(p2, 20) == 16);
    CHECK(AsioNearestLegalSize(p2, 1) == 8);
    CHECK(AsioNearestLegalSize(p2, 100000) == 2048);
    CHECK(AsioNearestLegalSize({}, 64) == 64);   // no list: request unchanged
}

int main() {
    TestLegalSizes();
    TestNearestLegalSize();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("asio host tests passed\n");
    return 0;
}
