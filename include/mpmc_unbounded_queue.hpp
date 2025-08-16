#ifndef MPMC_UNBOUNDED_QUEUE_HPP
#define MPMC_UNBOUNDED_QUEUE_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

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

	bool empty() const;
	std::size_t size_approx() const noexcept;

private:
	struct Node {
		std::shared_ptr<T> data;
		std::unique_ptr<Node> next;
	};

	Node* get_tail() const;
	std::unique_ptr<Node> try_pop_head();
	std::unique_ptr<Node> wait_pop_head();

private:
	mutable std::mutex head_mutex_;
	mutable std::mutex tail_mutex_;
	std::unique_ptr<Node> head_;
	Node* tail_;
	std::condition_variable data_available_;
	std::atomic<bool> closed_;
	std::atomic<std::size_t> approximate_size_;
};

// Implementation

template <typename T>
MPMCUnboundedQueue<T>::MPMCUnboundedQueue()
	: head_(std::make_unique<Node>()),
	  tail_(head_.get()),
	  closed_(false),
	  approximate_size_(0) {}

template <typename T>
MPMCUnboundedQueue<T>::~MPMCUnboundedQueue() {
	close();
}

template <typename T>
bool MPMCUnboundedQueue<T>::push(const T& value) {
	auto new_data = std::make_shared<T>(value);
	auto new_node = std::make_unique<Node>();
	Node* new_tail = new_node.get();

	{
		std::lock_guard<std::mutex> lock(tail_mutex_);
		if (closed_.load(std::memory_order_acquire)) {
			return false;
		}
		tail_->data = std::move(new_data);
		tail_->next = std::move(new_node);
		tail_ = new_tail;
		approximate_size_.fetch_add(1, std::memory_order_relaxed);
	}
	data_available_.notify_one();
	return true;
}

template <typename T>
bool MPMCUnboundedQueue<T>::push(T&& value) {
	auto new_data = std::make_shared<T>(std::move(value));
	auto new_node = std::make_unique<Node>();
	Node* new_tail = new_node.get();

	{
		std::lock_guard<std::mutex> lock(tail_mutex_);
		if (closed_.load(std::memory_order_acquire)) {
			return false;
		}
		tail_->data = std::move(new_data);
		tail_->next = std::move(new_node);
		tail_ = new_tail;
		approximate_size_.fetch_add(1, std::memory_order_relaxed);
	}
	data_available_.notify_one();
	return true;
}

template <typename T>
template <class... Args>
bool MPMCUnboundedQueue<T>::emplace(Args&&... args) {
	auto new_data = std::make_shared<T>(std::forward<Args>(args)...);
	auto new_node = std::make_unique<Node>();
	Node* new_tail = new_node.get();

	{
		std::lock_guard<std::mutex> lock(tail_mutex_);
		if (closed_.load(std::memory_order_acquire)) {
			return false;
		}
		tail_->data = std::move(new_data);
		tail_->next = std::move(new_node);
		tail_ = new_tail;
		approximate_size_.fetch_add(1, std::memory_order_relaxed);
	}
	data_available_.notify_one();
	return true;
}

template <typename T>
typename MPMCUnboundedQueue<T>::Node* MPMCUnboundedQueue<T>::get_tail() const {
	std::lock_guard<std::mutex> lock(tail_mutex_);
	return tail_;
}

template <typename T>
std::unique_ptr<typename MPMCUnboundedQueue<T>::Node> MPMCUnboundedQueue<T>::try_pop_head() {
	std::lock_guard<std::mutex> head_lock(head_mutex_);
	if (head_.get() == get_tail()) {
		return nullptr;
	}
	auto old_head = std::move(head_);
	head_ = std::move(old_head->next);
	approximate_size_.fetch_sub(1, std::memory_order_relaxed);
	return old_head;
}

template <typename T>
std::unique_ptr<typename MPMCUnboundedQueue<T>::Node> MPMCUnboundedQueue<T>::wait_pop_head() {
	std::unique_lock<std::mutex> head_lock(head_mutex_);
	data_available_.wait(head_lock, [this] {
		return head_.get() != get_tail() || closed_.load(std::memory_order_acquire);
	});
	if (head_.get() == get_tail()) {
		return nullptr;
	}
	auto old_head = std::move(head_);
	head_ = std::move(old_head->next);
	approximate_size_.fetch_sub(1, std::memory_order_relaxed);
	return old_head;
}

template <typename T>
bool MPMCUnboundedQueue<T>::try_pop(T& out) {
	auto old_head = try_pop_head();
	if (!old_head) {
		return false;
	}
	out = std::move(*old_head->data);
	return true;
}

template <typename T>
std::optional<T> MPMCUnboundedQueue<T>::try_pop() {
	auto old_head = try_pop_head();
	if (!old_head) {
		return std::nullopt;
	}
	return std::move(*old_head->data);
}

template <typename T>
bool MPMCUnboundedQueue<T>::wait_and_pop(T& out) {
	auto old_head = wait_pop_head();
	if (!old_head) {
		return false;
	}
	out = std::move(*old_head->data);
	return true;
}

template <typename T>
std::optional<T> MPMCUnboundedQueue<T>::wait_and_pop() {
	auto old_head = wait_pop_head();
	if (!old_head) {
		return std::nullopt;
	}
	return std::move(*old_head->data);
}

template <typename T>
void MPMCUnboundedQueue<T>::close() {
	bool expected = false;
	if (closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		data_available_.notify_all();
	}
}

template <typename T>
bool MPMCUnboundedQueue<T>::is_closed() const noexcept {
	return closed_.load(std::memory_order_acquire);
}

template <typename T>
bool MPMCUnboundedQueue<T>::empty() const {
	std::lock_guard<std::mutex> head_lock(head_mutex_);
	return (head_.get() == get_tail());
}

template <typename T>
std::size_t MPMCUnboundedQueue<T>::size_approx() const noexcept {
	return approximate_size_.load(std::memory_order_relaxed);
}

#endif // MPMC_UNBOUNDED_QUEUE_HPP