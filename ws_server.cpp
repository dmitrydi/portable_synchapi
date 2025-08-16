#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/signal_set.hpp>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

// Simple token-bucket rate limiter shared per IP
class TokenBucketLimiter {
public:
	TokenBucketLimiter(double tokens_per_second, double max_tokens)
		: tokens_per_second_(tokens_per_second),
		  max_tokens_(max_tokens),
		  available_tokens_(max_tokens),
		  last_refill_time_(std::chrono::steady_clock::now()),
		  last_seen_(std::chrono::steady_clock::now()) {}

	// Returns true if allowed (token consumed), false if limit exceeded
	bool allow_and_consume(double tokens = 1.0) {
		std::lock_guard<std::mutex> lock(mutex_);
		refill_locked();
		last_seen_ = std::chrono::steady_clock::now();
		if (available_tokens_ >= tokens) {
			available_tokens_ -= tokens;
			return true;
		}
		return false;
	}

	void set_seen_now() {
		std::lock_guard<std::mutex> lock(mutex_);
		last_seen_ = std::chrono::steady_clock::now();
	}

	std::chrono::steady_clock::time_point last_seen() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return last_seen_;
	}

private:
	void refill_locked() {
		auto now = std::chrono::steady_clock::now();
		double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_refill_time_).count();
		if (elapsed > 0) {
			available_tokens_ = std::min(max_tokens_, available_tokens_ + elapsed * tokens_per_second_);
			last_refill_time_ = now;
		}
	}

	mutable std::mutex mutex_;
	double tokens_per_second_;
	double max_tokens_;
	double available_tokens_;
	std::chrono::steady_clock::time_point last_refill_time_;
	std::chrono::steady_clock::time_point last_seen_;
};

// Registry mapping IP -> limiter (thread-safe)
class IpLimiterRegistry : public std::enable_shared_from_this<IpLimiterRegistry> {
public:
	IpLimiterRegistry(double tokens_per_second, double burst_tokens)
		: rate_(tokens_per_second), burst_(burst_tokens) {}

	std::shared_ptr<TokenBucketLimiter> get_or_create(const std::string& ip) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = ip_to_limiter_.find(ip);
		if (it != ip_to_limiter_.end()) {
			return it->second;
		}
		auto limiter = std::make_shared<TokenBucketLimiter>(rate_, burst_);
		ip_to_limiter_.emplace(ip, limiter);
		return limiter;
	}

	void cleanup_stale(std::chrono::seconds ttl) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto now = std::chrono::steady_clock::now();
		for (auto it = ip_to_limiter_.begin(); it != ip_to_limiter_.end();) {
			auto limiter = it->second;
			auto last_seen = limiter->last_seen();
			if (now - last_seen > ttl) {
				it = ip_to_limiter_.erase(it);
			} else {
				++it;
			}
		}
	}

private:
	double rate_;
	double burst_;
	std::mutex mutex_;
	std::unordered_map<std::string, std::shared_ptr<TokenBucketLimiter>> ip_to_limiter_;
};

// Echo WebSocket session with rate limiting
class WebsocketSession : public std::enable_shared_from_this<WebsocketSession> {
public:
	WebsocketSession(tcp::socket socket, std::shared_ptr<IpLimiterRegistry> registry)
		: ws_(std::move(socket)), registry_(std::move(registry)) {}

	void run() {
		// Set suggested timeout settings for the websocket
		ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
		// Accept the WebSocket handshake
		auto self = shared_from_this();
		ws_.async_accept(
			[self](beast::error_code ec) {
				self->on_accept(ec);
			});
	}

private:
	void on_accept(beast::error_code ec) {
		if (ec) {
			fail(ec, "accept");
			return;
		}

		// Identify remote IP
		beast::error_code ep_ec;
		auto ep = ws_.next_layer().socket().remote_endpoint(ep_ec);
		if (ep_ec) {
			fail(ep_ec, "remote_endpoint");
			ip_ = "unknown";
		} else {
			ip_ = ep.address().to_string();
		}
		limiter_ = registry_->get_or_create(ip_);
		if (limiter_) limiter_->set_seen_now();

		read_loop();
	}

	void read_loop() {
		auto self = shared_from_this();
		ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t bytes_transferred) {
			boost::ignore_unused(bytes_transferred);
			if (ec == websocket::error::closed) {
				return; // normal close
			}
			if (ec) {
				fail(ec, "read");
				return;
			}

			// Rate limit check: one token per message
			if (self->limiter_ && !self->limiter_->allow_and_consume(1.0)) {
				websocket::close_reason cr(websocket::close_code::policy_error);
				cr.reason = "rate limit exceeded";
				beast::error_code close_ec;
				self->ws_.close(cr, close_ec);
				return;
			}

			// Echo back the message
			self->do_write();
		});
	}

	void do_write() {
		auto self = shared_from_this();
		ws_.text(ws_.got_text());
		ws_.async_write(buffer_.data(), [self](beast::error_code ec, std::size_t bytes_transferred) {
			boost::ignore_unused(bytes_transferred);
			if (ec) {
				fail(ec, "write");
				return;
			}
			self->buffer_.consume(self->buffer_.size());
			self->read_loop();
		});
	}

	static void fail(beast::error_code ec, char const* what) {
		std::cerr << what << ": " << ec.message() << std::endl;
	}

	websocket::stream<beast::tcp_stream> ws_;
	beast::flat_buffer buffer_;
	std::shared_ptr<IpLimiterRegistry> registry_;
	std::shared_ptr<TokenBucketLimiter> limiter_;
	std::string ip_;
};

// Accepts incoming connections and launches the sessions
class Listener : public std::enable_shared_from_this<Listener> {
public:
	Listener(net::io_context& ioc, tcp::endpoint endpoint, std::shared_ptr<IpLimiterRegistry> registry)
		: ioc_(ioc), acceptor_(net::make_strand(ioc)), registry_(std::move(registry)), cleanup_timer_(ioc) {
		beast::error_code ec;

		// Open the acceptor
		acceptor_.open(endpoint.protocol(), ec);
		if (ec) {
			fail(ec, "open");
			return;
		}

		// Allow address reuse
		acceptor_.set_option(net::socket_base::reuse_address(true), ec);
		if (ec) {
			fail(ec, "set_option");
			return;
		}

		// Bind to the server address
		acceptor_.bind(endpoint, ec);
		if (ec) {
			fail(ec, "bind");
			return;
		}

		// Start listening for connections
		acceptor_.listen(net::socket_base::max_listen_connections, ec);
		if (ec) {
			fail(ec, "listen");
			return;
		}
	}

	void run() {
		do_accept();
		// start periodic cleanup every 60 seconds
		schedule_cleanup();
	}

private:
	void do_accept() {
		auto self = shared_from_this();
		acceptor_.async_accept(net::make_strand(ioc_), [self](beast::error_code ec, tcp::socket socket) {
			if (ec) {
				fail(ec, "accept");
			} else {
				std::make_shared<WebsocketSession>(std::move(socket), self->registry_)->run();
			}
			self->do_accept();
		});
	}

	void schedule_cleanup() {
		auto self = shared_from_this();
		cleanup_timer_.expires_after(std::chrono::seconds(60));
		cleanup_timer_.async_wait([self](beast::error_code ec) {
			if (!ec) {
				self->registry_->cleanup_stale(std::chrono::seconds(300));
				self->schedule_cleanup();
			}
		});
	}

	static void fail(beast::error_code ec, char const* what) {
		std::cerr << what << ": " << ec.message() << std::endl;
	}

	net::io_context& ioc_;
	tcp::acceptor acceptor_;
	std::shared_ptr<IpLimiterRegistry> registry_;
	net::steady_timer cleanup_timer_;
};

struct Config {
	std::string address = "0.0.0.0";
	unsigned short port = 9002;
	double rate = 20.0;   // messages per second per IP
	double burst = 40.0;  // maximum burst tokens per IP
	int threads = 1;
};

static std::optional<Config> parse_args(int argc, char** argv) {
	Config cfg;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		auto require_value = [&](const char* name) -> const char* {
			if (i + 1 >= argc) {
				std::cerr << "Missing value for " << name << std::endl;
				return nullptr;
			}
			return argv[++i];
		};
		if (arg == "--address") {
			const char* v = require_value("--address");
			if (!v) return std::nullopt;
			cfg.address = v;
		} else if (arg == "--port") {
			const char* v = require_value("--port");
			if (!v) return std::nullopt;
			cfg.port = static_cast<unsigned short>(std::stoi(v));
		} else if (arg == "--rate") {
			const char* v = require_value("--rate");
			if (!v) return std::nullopt;
			cfg.rate = std::stod(v);
		} else if (arg == "--burst") {
			const char* v = require_value("--burst");
			if (!v) return std::nullopt;
			cfg.burst = std::stod(v);
		} else if (arg == "--threads") {
			const char* v = require_value("--threads");
			if (!v) return std::nullopt;
			cfg.threads = std::max(1, std::stoi(v));
		} else if (arg == "--help" || arg == "-h") {
			std::cout << "Usage: ws_server [--address 0.0.0.0] [--port 9002] [--rate 20] [--burst 40] [--threads 1]\n";
			return std::nullopt;
		}
	}
	return cfg;
}

int main(int argc, char** argv) {
	auto cfgOpt = parse_args(argc, argv);
	if (!cfgOpt.has_value()) return 0;
	Config cfg = *cfgOpt;

	try {
		net::io_context ioc{cfg.threads};

		auto registry = std::make_shared<IpLimiterRegistry>(cfg.rate, cfg.burst);

		auto const address = net::ip::make_address(cfg.address);
		auto const endpoint = tcp::endpoint{address, cfg.port};

		net::signal_set signals(ioc, SIGINT, SIGTERM);
		signals.async_wait([&](beast::error_code const&, int){ ioc.stop(); });

		std::make_shared<Listener>(ioc, endpoint, registry)->run();

		std::vector<std::thread> v;
		v.reserve(std::max(0, cfg.threads - 1));
		for (int i = 0; i < cfg.threads - 1; ++i) {
			v.emplace_back([&ioc]{ ioc.run(); });
		}
		ioc.run();
		for (auto& t : v) t.join();
	} catch (std::exception const& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}