// SPDX-License-Identifier: Apache-2.0
#include "in_memory_transport.hpp"

#include "mcp/log.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace mcp::test {

// One side's queue + flags + cv. Two of them in a Channel make a pair.
struct Endpoint {
    std::mutex                mu;
    std::condition_variable   cv;
    std::deque<std::string>   q;
    bool                      closed = false;
};

struct Channel {
    Endpoint a;  // A consumes from here
    Endpoint b;  // B consumes from here
};

namespace {

inline Endpoint& endpoint_for(Channel& ch, InMemoryTransport::Side s) {
    return s == InMemoryTransport::Side::kA ? ch.a : ch.b;
}

inline Endpoint& peer_endpoint(Channel& ch, InMemoryTransport::Side s) {
    return s == InMemoryTransport::Side::kA ? ch.b : ch.a;
}

}  // namespace

InMemoryTransport::InMemoryTransport(Side side, std::shared_ptr<Channel> ch)
    : side_(side), ch_(std::move(ch)) {}

InMemoryTransport::~InMemoryTransport() {
    close();
}

void InMemoryTransport::on_message(MessageCallback cb) { on_message_ = std::move(cb); }
void InMemoryTransport::on_error(ErrorCallback cb)     { on_error_   = std::move(cb); }
void InMemoryTransport::on_close(CloseCallback cb)     { on_close_   = std::move(cb); }

void InMemoryTransport::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        return;
    }
    closed_.store(false, std::memory_order_release);
    worker_ = std::thread([this] { run_dispatch(); });
}

void InMemoryTransport::close() {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
        return;
    }
    {
        // Wake our own dispatch loop.
        auto& ours = endpoint_for(*ch_, side_);
        std::lock_guard<std::mutex> lk(ours.mu);
        ours.closed = true;
        ours.cv.notify_all();
    }
    {
        // Tell the peer we're gone so its `send()` fails and its dispatch
        // exits when its queue drains.
        auto& peer = peer_endpoint(*ch_, side_);
        std::lock_guard<std::mutex> lk(peer.mu);
        peer.closed = true;  // peer's "incoming from us" tap is closed
        peer.cv.notify_all();
    }
    if (worker_.joinable()) worker_.join();

    if (on_close_) {
        try { on_close_(); }
        catch (const std::exception& e) {
            MCP_LOG_ERROR(std::string{"on_close threw: "} + e.what());
        }
    }
}

std::error_code InMemoryTransport::send(std::string_view frame) {
    if (!started_.load(std::memory_order_acquire) ||
         closed_.load(std::memory_order_acquire)) {
        return std::make_error_code(std::errc::not_connected);
    }
    auto& peer = peer_endpoint(*ch_, side_);
    std::lock_guard<std::mutex> lk(peer.mu);
    if (peer.closed) {
        return std::make_error_code(std::errc::not_connected);
    }
    peer.q.emplace_back(frame);
    peer.cv.notify_one();
    return {};
}

bool InMemoryTransport::is_open() const noexcept {
    return started_.load(std::memory_order_acquire) &&
           !closed_.load(std::memory_order_acquire);
}

void InMemoryTransport::run_dispatch() {
    auto& ours = endpoint_for(*ch_, side_);
    while (true) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lk(ours.mu);
            ours.cv.wait(lk, [&] { return ours.closed || !ours.q.empty(); });
            if (ours.closed && ours.q.empty()) return;
            frame = std::move(ours.q.front());
            ours.q.pop_front();
        }
        if (on_message_) {
            try { on_message_(std::move(frame)); }
            catch (const std::exception& e) {
                MCP_LOG_ERROR(std::string{"on_message threw: "} + e.what());
            }
        }
    }
}

InMemoryTransportPair make_in_memory_pair() {
    auto ch = std::make_shared<Channel>();
    return {
        std::make_unique<InMemoryTransport>(InMemoryTransport::Side::kA, ch),
        std::make_unique<InMemoryTransport>(InMemoryTransport::Side::kB, ch),
    };
}

}  // namespace mcp::test
