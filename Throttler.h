#ifndef THROTTLER_H
#define THROTTLER_H

#include <chrono>
#include <deque>

class Throttler {
public:
    explicit Throttler(unsigned max_rate)
        : maxRate(max_rate) {}

    bool add(std::chrono::steady_clock::time_point tp) {
        const auto windowStart = tp - std::chrono::seconds(1);

        // Remove timestamps that are outside the [tp - 1s, tp] window
        while (!recentCalls.empty() && recentCalls.front() < windowStart) {
            recentCalls.pop_front();
        }

        if (recentCalls.size() < maxRate) {
            recentCalls.push_back(tp);
            return true;
        }
        return false;
    }

private:
    const unsigned maxRate;
    std::deque<std::chrono::steady_clock::time_point> recentCalls;
};

#endif // THROTTLER_H

