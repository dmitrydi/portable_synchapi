#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "../include/mpmc_unbounded_queue.hpp"

int main() {
	MPMCUnboundedQueue<int> queue;

	constexpr int kNumProducers = 4;
	constexpr int kNumConsumers = 4;
	constexpr int kItemsPerProducer = 25000;
	constexpr int kTotalItems = kNumProducers * kItemsPerProducer;

	std::atomic<int> produced{0};
	std::atomic<int> consumed{0};

	std::vector<std::thread> producers;
	producers.reserve(kNumProducers);
	for (int p = 0; p < kNumProducers; ++p) {
		producers.emplace_back([p, &queue, &produced]() {
			for (int i = 0; i < kItemsPerProducer; ++i) {
				queue.push(p * kItemsPerProducer + i);
				produced.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	std::vector<std::thread> consumers;
	consumers.reserve(kNumConsumers);
	for (int c = 0; c < kNumConsumers; ++c) {
		consumers.emplace_back([c, &queue, &consumed]() {
			int value = 0;
			while (true) {
				if (queue.wait_and_pop(value)) {
					consumed.fetch_add(1, std::memory_order_relaxed);
				} else {
					break;
				}
			}
		});
	}

	for (auto& t : producers) {
		t.join();
	}

	queue.close();

	for (auto& t : consumers) {
		t.join();
	}

	std::cout << "Produced: " << produced.load() << ", Consumed: " << consumed.load() << "\n";
	std::cout << (consumed.load() == kTotalItems ? "OK" : "MISMATCH") << "\n";

	if (consumed.load() != kTotalItems) {
		return 1;
	}
	return 0;
}