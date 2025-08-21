#include <chrono>
#include <iostream>
#include <thread>

#include "rate_throttler.h"

int main() {
	RateThrottler throttler(5); // allow up to 5 per second

	// Make 5 immediate calls that should be allowed
	for (int i = 0; i < 5; ++i) {
		bool ok = throttler.available(std::chrono::steady_clock::now());
		std::cout << "call " << i + 1 << ": " << (ok ? "allowed" : "blocked") << "\n";
	}

	// 6th immediate call should be blocked
	bool sixth = throttler.available(std::chrono::steady_clock::now());
	std::cout << "call 6: " << (sixth ? "allowed" : "blocked") << "\n";

	// Old timestamp should bypass throttling
	auto old_tp = std::chrono::steady_clock::now() - std::chrono::seconds(2);
	bool old_ok = throttler.available(old_tp);
	std::cout << "old timestamp call: " << (old_ok ? "allowed" : "blocked") << "\n";

	// Wait to refill and try again
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	bool after_wait = throttler.available(std::chrono::steady_clock::now());
	std::cout << "after wait call: " << (after_wait ? "allowed" : "blocked") << "\n";

	return 0;
}

