// SPDX-License-Identifier: Apache-2.0
//
// In-memory paired transport for tests. A shared `Channel` holds two
// in/out queues; each `InMemoryTransport` is a unique-owned end on top
// of the channel and is therefore movable into a Session.

#pragma once

#include "mcp/transport.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace mcp::test {

class InMemoryTransport;

struct Channel;  // forward

struct InMemoryTransportPair {
    std::unique_ptr<InMemoryTransport> a;
    std::unique_ptr<InMemoryTransport> b;
};

InMemoryTransportPair make_in_memory_pair();

class InMemoryTransport final : public Transport {
public:
    enum class Side : int { kA = 0, kB = 1 };

    InMemoryTransport(Side side, std::shared_ptr<Channel> ch);
    ~InMemoryTransport() override;

    void on_message(MessageCallback cb) override;
    void on_error(ErrorCallback cb)     override;
    void on_close(CloseCallback cb)     override;
    void start() override;
    void close() override;
    [[nodiscard]] std::error_code send(std::string_view frame) override;
    [[nodiscard]] bool is_open() const noexcept override;

private:
    void run_dispatch();

    Side                       side_;
    std::shared_ptr<Channel>   ch_;

    std::atomic<bool>          started_{false};
    std::atomic<bool>          closed_{false};

    std::thread                worker_;

    MessageCallback            on_message_;
    ErrorCallback              on_error_;
    CloseCallback              on_close_;
};

}  // namespace mcp::test
