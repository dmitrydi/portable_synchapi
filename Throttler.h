#ifndef THROTTLER_H
#define THROTTLER_H

#include <chrono>
#include <vector>

class Throttler {
public:
    explicit Throttler(unsigned max_rate)
        : maxRate(max_rate), ring(max_rate), count(0), start(0) {}

    // O(1) worst-case time: maintains timestamps of accepted calls in a ring buffer.
    // If the buffer is full, we allow only if the oldest accepted call is strictly older than 1s.
    bool add(std::chrono::steady_clock::time_point tp) {
        if (maxRate == 0) {
            return false;
        }

        if (count < maxRate) {
            const unsigned insertIndex = (start + count) % maxRate;
            ring[insertIndex] = tp;
            ++count;
            return true;
        } else {
            const auto oldest = ring[start];
            if (tp - oldest > std::chrono::seconds(1)) {
                ring[start] = tp; // Replace the oldest accepted call
                start = (start + 1) % maxRate;
                return true;
            }
            return false;
        }
    }

private:
    const unsigned maxRate;
    std::vector<std::chrono::steady_clock::time_point> ring; // size: maxRate
    unsigned count;  // number of valid entries, <= maxRate
    unsigned start;  // index of the oldest entry when count == maxRate
};

#endif // THROTTLER_H

