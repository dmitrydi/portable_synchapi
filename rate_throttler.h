#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>

class RateThrottler {
public:
	explicit RateThrottler(unsigned max_rate)
		: maxRatePerSecond_(static_cast<double>(max_rate)),
		  availableTokens_(static_cast<double>(max_rate)),
		  lastRefillTime_(std::chrono::steady_clock::now()) {}

	bool available(std::chrono::steady_clock::time_point tp) {
		auto now = std::chrono::steady_clock::now();
		// Bypass throttling for timestamps older than 1 second before now
		if (tp < now - std::chrono::seconds(1)) {
			return true;
		}

		std::lock_guard<std::mutex> lock(mutex_);

		// Refill tokens based on time elapsed since last refill
		const auto elapsed = std::chrono::duration<double>(now - lastRefillTime_).count();
		if (elapsed > 0.0) {
			const double refill = elapsed * maxRatePerSecond_;
			availableTokens_ = std::min(maxRatePerSecond_, availableTokens_ + refill);
			lastRefillTime_ = now;
		}

		if (availableTokens_ >= 1.0) {
			availableTokens_ -= 1.0;
			return true;
		}
		return false;
	}

private:
	const double maxRatePerSecond_;
	double availableTokens_;
	std::chrono::steady_clock::time_point lastRefillTime_;
	std::mutex mutex_;
};

