#include <iostream>
#include <chrono>
#include "Throttler.h"

using SteadyClock = std::chrono::steady_clock;
using namespace std::chrono;

int main() {
    Throttler t(3);

    const SteadyClock::time_point base{}; // epoch start for deterministic tests

    // First 3 calls within 1s should pass
    bool r1 = t.add(base + milliseconds(0));
    bool r2 = t.add(base + milliseconds(100));
    bool r3 = t.add(base + milliseconds(200));

    // 4th within 1s window should fail
    bool r4 = t.add(base + milliseconds(300));

    // After window slides past the first call (> 1000ms later), it should pass
    bool r5 = t.add(base + milliseconds(1001));

    std::cout << std::boolalpha
              << "r1=" << r1 << " r2=" << r2 << " r3=" << r3
              << " r4=" << r4 << " r5=" << r5 << "\n";

    // Expect: true true true false true
    if (!(r1 && r2 && r3 && !r4 && r5)) {
        std::cerr << "Test failed" << std::endl;
        return 1;
    }

    std::cout << "All tests passed" << std::endl;
    return 0;
}

