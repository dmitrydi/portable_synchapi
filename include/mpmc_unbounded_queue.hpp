#ifndef MPMC_UNBOUNDED_QUEUE_HPP
#define MPMC_UNBOUNDED_QUEUE_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#include <stdexcept>

// Lock-free MPMC unbounded queue (Michael-Scott algorithm)
// - Core enqueue/dequeue are lock-free
// - Blocking waits use a separate condition variable that does not guard the queue (does not break lock-freedom)
// - Memory reclamation uses a minimal hazard-pointer scheme (header-only)

namespace mpmc_detail {

	constexpr unsigned kMaxHazardPointers = 128;

	struct HazardPointerSlot {
		std::atomic<std::thread::id> owner_id;
		std::atomic<void*> pointer;
	};

	inline HazardPointerSlot g_hazard_slots[kMaxHazardPointers] = {};

	struct HazardPointerHolder {
		HazardPointerSlot* slot;

		HazardPointerHolder() : slot(nullptr) {
			std::thread::id this_id = std::this_thread::get_id();
			for (unsigned i = 0; i < kMaxHazardPointers; ++i) {
				std::thread::id empty_id; // default constructed (no thread)
				if (g_hazard_slots[i].owner_id.compare_exchange_strong(empty_id, this_id, std::memory_order_acq_rel)) {
					slot = &g_hazard_slots[i];
					break;
				}
			}
			if (!slot) {
				throw std::runtime_error("No free hazard pointer slots");
			}
		}

		~HazardPointerHolder() {
			if (slot) {
				slot->pointer.store(nullptr, std::memory_order_release);
				slot->owner_id.store(std::thread::id{}, std::memory_order_release);
			}
		}

		void* protect(void* p) {
			slot->pointer.store(p, std::memory_order_release);
			return p;
		}

		void clear() {
			slot->pointer.store(nullptr, std::memory_order_release);
		}
	};

	inline bool any_hazard_points_to(void* p) {
		for (unsigned i = 0; i < kMaxHazardPointers; ++i) {
			if (g_hazard_slots[i].pointer.load(std::memory_order_acquire) == p) {
				return true;
			}
		}
		return false;
	}

	struct RetiredNode {
		void* pointer;
		RetiredNode* next;
		explicit RetiredNode(void* p) : pointer(p), next(nullptr) {}
	};

	inline void add_to_retires(RetiredNode*& list_head, void* p) {
		RetiredNode* node = new RetiredNode(p);
		node->next = list_head;
		list_head = node;
	}

	inline void delete_retired_nodes(RetiredNode*& list_head) {
		RetiredNode* current = list_head;
		while (current) {
			RetiredNode* next = current->next;
			delete static_cast<char*>(current->pointer); // placeholder, actual delete handled by callers
			delete current;
			current = next;
		}
		list_head = nullptr;
	}

}

template <typename T>
class MPMCUnboundedQueue {
public:
	MPMCUnboundedQueue();
	~MPMCUnboundedQueue();

	MPMCUnboundedQueue(const MPMCUnboundedQueue&) = delete;
	MPMCUnboundedQueue& operator=(const MPMCUnboundedQueue&) = delete;
	MPMCUnboundedQueue(MPMCUnboundedQueue&&) = delete;
	MPMCUnboundedQueue& operator=(MPMCUnboundedQueue&&) = delete;

	bool push(const T& value);
	bool push(T&& value);

	template <class... Args>
	bool emplace(Args&&... args);

	bool try_pop(T& out);
	std::optional<T> try_pop();

	bool wait_and_pop(T& out);
	std::optional<T> wait_and_pop();

	void close();
	bool is_closed() const noexcept;

	bool empty() const noexcept;
	std::size_t size_approx() const noexcept;

private:
	struct Node {
		std::unique_ptr<T> data;
		std::atomic<Node*> next;

		Node() : data(nullptr), next(nullptr) {}
		explicit Node(std::unique_ptr<T> d) : data(std::move(d)), next(nullptr) {}
	};

	static void delete_node(Node* n) noexcept {
		delete n;
	}

	bool enqueue_node(std::unique_ptr<T> data);
	bool dequeue_node(std::unique_ptr<T>& out_data);

	void reclaim_later(Node* n);
	void delete_nodes_with_no_hazard();

private:
	std::atomic<Node*> head_;
	std::atomic<Node*> tail_;
	std::atomic<bool> closed_;
	std::atomic<std::size_t> approximate_size_;

	mutable std::mutex wait_mutex_;
	std::condition_variable wait_cv_;

	// Per-thread retired list for hazard-pointer reclamation
	struct ThreadRetireList {
		mpmc_detail::RetiredNode* head;
		std::size_t count;
		ThreadRetireList() : head(nullptr), count(0) {}
	};
	static thread_local ThreadRetireList retired_;
};

// Static thread_local definition

template <typename T>
thread_local typename MPMCUnboundedQueue<T>::ThreadRetireList MPMCUnboundedQueue<T>::retired_{};

// Implementation

template <typename T>
MPMCUnboundedQueue<T>::MPMCUnboundedQueue()
	: head_(new Node()),
	  tail_(head_.load(std::memory_order_relaxed)),
	  closed_(false),
	  approximate_size_(0) {}

template <typename T>
MPMCUnboundedQueue<T>::~MPMCUnboundedQueue() {
	close();
	// Best-effort cleanup: traverse from current head and delete remaining nodes
	Node* node = head_.load(std::memory_order_acquire);
	while (node) {
		Node* next = node->next.load(std::memory_order_acquire);
		delete_node(node);
		node = next;
	}
	// Delete any retired nodes owned by this thread (others may remain until thread exit)
	mpmc_detail::RetiredNode* current = retired_.head;
	while (current) {
		mpmc_detail::RetiredNode* next = current->next;
		delete_node(static_cast<Node*>(current->pointer));
		delete current;
		current = next;
	}
	retired_.head = nullptr;
	retired_.count = 0;
}

template <typename T>
bool MPMCUnboundedQueue<T>::enqueue_node(std::unique_ptr<T> data) {
	if (closed_.load(std::memory_order_acquire)) {
		return false;
	}
	Node* new_node = new Node(std::move(data));
	new_node->next.store(nullptr, std::memory_order_relaxed);

	mpmc_detail::HazardPointerHolder hp; // protect 'last' while dereferencing it
	while (true) {
		Node* last = tail_.load(std::memory_order_acquire);
		for (;;) {
			Node* observed = last;
			hp.protect(last);
			last = tail_.load(std::memory_order_acquire);
			if (last == observed) break;
		}
		Node* next = last->next.load(std::memory_order_acquire);
		if (last == tail_.load(std::memory_order_acquire)) {
			if (next == nullptr) {
				if (last->next.compare_exchange_weak(next, new_node, std::memory_order_release, std::memory_order_relaxed)) {
					tail_.compare_exchange_strong(last, new_node, std::memory_order_release, std::memory_order_relaxed);
					hp.clear();
					break;
				}
			} else {
				tail_.compare_exchange_weak(last, next, std::memory_order_release, std::memory_order_relaxed);
			}
		}
	}
	approximate_size_.fetch_add(1, std::memory_order_relaxed);
	wait_cv_.notify_one();
	return true;
}

template <typename T>
bool MPMCUnboundedQueue<T>::push(const T& value) {
	return enqueue_node(std::make_unique<T>(value));
}

template <typename T>
bool MPMCUnboundedQueue<T>::push(T&& value) {
	return enqueue_node(std::make_unique<T>(std::move(value)));
}

template <typename T>
template <class... Args>
bool MPMCUnboundedQueue<T>::emplace(Args&&... args) {
	return enqueue_node(std::make_unique<T>(std::forward<Args>(args)...));
}

template <typename T>
void MPMCUnboundedQueue<T>::reclaim_later(Node* n) {
	// Retire node for later reclamation once no hazard pointer references it
	auto* entry = new mpmc_detail::RetiredNode(static_cast<void*>(n));
	entry->next = retired_.head;
	retired_.head = entry;
	std::size_t new_count = ++retired_.count;
	if (new_count >= 64) {
		delete_nodes_with_no_hazard();
	}
}

template <typename T>
void MPMCUnboundedQueue<T>::delete_nodes_with_no_hazard() {
	mpmc_detail::RetiredNode** current = &retired_.head;
	while (*current) {
		void* p = (*current)->pointer;
		if (!mpmc_detail::any_hazard_points_to(p)) {
			mpmc_detail::RetiredNode* old = *current;
			*current = old->next;
			delete_node(static_cast<Node*>(p));
			delete old;
			--retired_.count;
		} else {
			current = &((*current)->next);
		}
	}
}

template <typename T>
bool MPMCUnboundedQueue<T>::dequeue_node(std::unique_ptr<T>& out_data) {
	mpmc_detail::HazardPointerHolder hp;
	while (true) {
		Node* first = head_.load(std::memory_order_acquire);
		// Protect the observed head with hazard pointer and re-read until stable
		for (;;) {
			Node* temp = first;
			hp.protect(first);
			first = head_.load(std::memory_order_acquire);
			if (first == temp) break;
		}

		Node* last = tail_.load(std::memory_order_acquire);
		Node* next = first->next.load(std::memory_order_acquire);

		if (first == head_.load(std::memory_order_acquire)) {
			if (first == last) {
				if (next == nullptr) {
					hp.clear();
					return false; // empty
				}
				// Tail is falling behind, try to advance it
				tail_.compare_exchange_weak(last, next, std::memory_order_release, std::memory_order_relaxed);
			} else {
				if (next == nullptr) {
					continue; // Should not happen often
				}
				// The value to pop resides in next->data
				if (head_.compare_exchange_weak(first, next, std::memory_order_acquire, std::memory_order_relaxed)) {
					// We have logically removed 'first' (old dummy). Safe to read data from 'next'.
					out_data = std::move(next->data);
					approximate_size_.fetch_sub(1, std::memory_order_relaxed);
					hp.clear();
					reclaim_later(first);
					return true;
				}
			}
		}
	}
}

template <typename T>
bool MPMCUnboundedQueue<T>::try_pop(T& out) {
	std::unique_ptr<T> data;
	bool ok = dequeue_node(data);
	if (!ok) return false;
	out = std::move(*data);
	return true;
}

template <typename T>
std::optional<T> MPMCUnboundedQueue<T>::try_pop() {
	std::unique_ptr<T> data;
	if (!dequeue_node(data)) return std::nullopt;
	return std::move(*data);
}

template <typename T>
bool MPMCUnboundedQueue<T>::wait_and_pop(T& out) {
	for (;;) {
		if (try_pop(out)) return true;
		std::unique_lock<std::mutex> lk(wait_mutex_);
		wait_cv_.wait(lk, [this] {
			return closed_.load(std::memory_order_acquire) || !empty();
		});
		if (closed_.load(std::memory_order_acquire) && empty()) {
			return false;
		}
	}
}

template <typename T>
std::optional<T> MPMCUnboundedQueue<T>::wait_and_pop() {
	T value;
	if (!wait_and_pop(value)) return std::nullopt;
	return value;
}

template <typename T>
void MPMCUnboundedQueue<T>::close() {
	bool expected = false;
	if (closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		wait_cv_.notify_all();
	}
}

template <typename T>
bool MPMCUnboundedQueue<T>::is_closed() const noexcept {
	return closed_.load(std::memory_order_acquire);
}

template <typename T>
bool MPMCUnboundedQueue<T>::empty() const noexcept {
	return approximate_size_.load(std::memory_order_relaxed) == 0;
}

template <typename T>
std::size_t MPMCUnboundedQueue<T>::size_approx() const noexcept {
	return approximate_size_.load(std::memory_order_relaxed);
}

#endif // MPMC_UNBOUNDED_QUEUE_HPP