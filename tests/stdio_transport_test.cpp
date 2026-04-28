// SPDX-License-Identifier: Apache-2.0
//
// Pipe-based tests for StdioTransport. Each test wires up a pair of
// pipes as if we were sitting between two processes, then drives the
// transport directly without ever touching the test binary's real stdio.

#include "mcp/stdio_transport.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// Helper that holds two pipes (send → transport, transport → us) and a
// configured StdioTransport ready to start.
struct PipeHarness {
    int  to_transport_r = -1;
    int  to_transport_w = -1;
    int  from_transport_r = -1;
    int  from_transport_w = -1;
    std::unique_ptr<mcp::StdioTransport> transport;

    PipeHarness() {
        int t2[2];
        int f2[2];
        EXPECT_EQ(::pipe(t2), 0);
        EXPECT_EQ(::pipe(f2), 0);
        to_transport_r = t2[0];
        to_transport_w = t2[1];
        from_transport_r = f2[0];
        from_transport_w = f2[1];
        mcp::StdioTransport::Options opts{};
        opts.read_fd  = to_transport_r;
        opts.write_fd = from_transport_w;
        opts.owns_fds = true;
        transport = std::make_unique<mcp::StdioTransport>(opts);
    }

    ~PipeHarness() {
        if (transport) transport->close();
        // The transport owns the read-end and write-end fds it was given,
        // so we just clean up the others.
        if (to_transport_w   >= 0) ::close(to_transport_w);
        if (from_transport_r >= 0) ::close(from_transport_r);
    }

    void write_to_transport(std::string_view s) {
        const char* p = s.data();
        std::size_t left = s.size();
        while (left > 0) {
            ssize_t n = ::write(to_transport_w, p, left);
            ASSERT_GT(n, 0);
            p += n; left -= static_cast<std::size_t>(n);
        }
    }

    /// Read up to `n` bytes from the transport's stdout pipe. Returns
    /// whatever was available; will block waiting for at least one byte.
    std::string read_from_transport(std::size_t n = 4096) {
        std::string out;
        out.resize(n);
        ssize_t got = ::read(from_transport_r, out.data(), n);
        if (got <= 0) return {};
        out.resize(static_cast<std::size_t>(got));
        return out;
    }

    void close_transport_input() {
        if (to_transport_w >= 0) {
            ::close(to_transport_w);
            to_transport_w = -1;
        }
    }
};

// Captures messages, errors, close events for a transport.
struct Sink {
    std::mutex                mu;
    std::condition_variable   cv;
    std::vector<std::string>  messages;
    std::vector<std::error_code> errors;
    bool                      closed = false;

    void on_message(std::string m) {
        std::lock_guard<std::mutex> lk(mu);
        messages.push_back(std::move(m));
        cv.notify_all();
    }
    void on_error(std::error_code ec) {
        std::lock_guard<std::mutex> lk(mu);
        errors.push_back(ec);
        cv.notify_all();
    }
    void on_close() {
        std::lock_guard<std::mutex> lk(mu);
        closed = true;
        cv.notify_all();
    }

    bool wait_for_messages(std::size_t want, std::chrono::milliseconds t = 2s) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, t, [&] { return messages.size() >= want; });
    }
    bool wait_for_close(std::chrono::milliseconds t = 2s) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, t, [&] { return closed; });
    }
};

// -------------------------------------------------------------------------

TEST(StdioTransport, DeliversWholeFrame) {
    PipeHarness h;
    Sink sink;
    h.transport->on_message([&](std::string s) { sink.on_message(std::move(s)); });
    h.transport->on_close ([&]                  { sink.on_close(); });
    h.transport->start();

    h.write_to_transport(std::string{R"({"jsonrpc":"2.0","id":1,"method":"x"})"} + "\n");

    ASSERT_TRUE(sink.wait_for_messages(1));
    EXPECT_EQ(sink.messages.front(), R"({"jsonrpc":"2.0","id":1,"method":"x"})");
}

TEST(StdioTransport, SplitsMultipleFramesByNewline) {
    PipeHarness h;
    Sink sink;
    h.transport->on_message([&](std::string s) { sink.on_message(std::move(s)); });
    h.transport->start();

    h.write_to_transport("a\nb\nc\n");

    ASSERT_TRUE(sink.wait_for_messages(3));
    EXPECT_EQ(sink.messages[0], "a");
    EXPECT_EQ(sink.messages[1], "b");
    EXPECT_EQ(sink.messages[2], "c");
}

TEST(StdioTransport, BuffersAcrossReads) {
    PipeHarness h;
    Sink sink;
    h.transport->on_message([&](std::string s) { sink.on_message(std::move(s)); });
    h.transport->start();

    h.write_to_transport("part");
    h.write_to_transport("ial\nthen-next\n");

    ASSERT_TRUE(sink.wait_for_messages(2));
    EXPECT_EQ(sink.messages[0], "partial");
    EXPECT_EQ(sink.messages[1], "then-next");
}

TEST(StdioTransport, IgnoresEmptyLines) {
    PipeHarness h;
    Sink sink;
    h.transport->on_message([&](std::string s) { sink.on_message(std::move(s)); });
    h.transport->start();

    h.write_to_transport("\n\nhi\n\n");

    ASSERT_TRUE(sink.wait_for_messages(1));
    EXPECT_EQ(sink.messages.size(), 1u);
    EXPECT_EQ(sink.messages[0], "hi");
}

TEST(StdioTransport, SendWritesNewlineDelimitedFrames) {
    PipeHarness h;
    h.transport->start();

    auto ec = h.transport->send(R"({"jsonrpc":"2.0","id":7,"method":"y"})");
    ASSERT_FALSE(ec) << ec.message();

    auto out = h.read_from_transport();
    EXPECT_EQ(out, std::string{R"({"jsonrpc":"2.0","id":7,"method":"y"})"} + "\n");
}

TEST(StdioTransport, RejectsFramesWithEmbeddedNewline) {
    PipeHarness h;
    h.transport->start();
    auto ec = h.transport->send("a\nb");
    EXPECT_TRUE(ec);
    EXPECT_EQ(ec, std::make_error_code(std::errc::invalid_argument));
}

TEST(StdioTransport, ConcurrentSendsAreSerialized) {
    PipeHarness h;
    h.transport->start();

    constexpr int kThreads = 8;
    constexpr int kPer     = 50;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t]() {
            std::string msg = "thread-" + std::to_string(t);
            for (int i = 0; i < kPer; ++i) {
                ASSERT_FALSE(h.transport->send(msg));
            }
        });
    }
    for (auto& th : ts) th.join();

    // Drain everything the transport wrote, line by line, and verify
    // every frame is one of the expected strings (no torn writes).
    std::string accumulated;
    while (true) {
        // Tiny non-blocking poll: read whatever's there, with a short
        // budget. We rely on the writes having completed before joins.
        char buf[4096];
        // Make read_from_transport non-blocking-ish by checking poll.
        struct pollfd pfd{ h.from_transport_r, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, 200);
        if (pr <= 0) break;
        ssize_t n = ::read(h.from_transport_r, buf, sizeof(buf));
        if (n <= 0) break;
        accumulated.append(buf, static_cast<std::size_t>(n));
    }

    int frame_count = 0;
    std::size_t pos = 0;
    while (pos < accumulated.size()) {
        std::size_t nl = accumulated.find('\n', pos);
        if (nl == std::string::npos) break;
        const std::string frame = accumulated.substr(pos, nl - pos);
        EXPECT_TRUE(frame.starts_with("thread-"));
        EXPECT_EQ(frame.find('\n'), std::string::npos);
        pos = nl + 1;
        ++frame_count;
    }
    EXPECT_EQ(frame_count, kThreads * kPer);
}

TEST(StdioTransport, SendBeforeStartFails) {
    PipeHarness h;
    auto ec = h.transport->send("x");
    EXPECT_TRUE(ec);
    EXPECT_EQ(ec, std::make_error_code(std::errc::not_connected));
}

TEST(StdioTransport, EofTriggersClose) {
    PipeHarness h;
    Sink sink;
    h.transport->on_close([&] { sink.on_close(); });
    h.transport->start();

    h.close_transport_input();  // EOF on the transport's read end.
    EXPECT_TRUE(sink.wait_for_close());
}

TEST(StdioTransport, ExplicitCloseUnblocksReadLoop) {
    PipeHarness h;
    Sink sink;
    h.transport->on_close([&] { sink.on_close(); });
    h.transport->start();

    // Read loop is parked in poll() with no input. close() must
    // wake it up via the self-pipe.
    h.transport->close();
    EXPECT_TRUE(sink.wait_for_close());
}

TEST(StdioTransport, IsOpenReflectsLifecycle) {
    PipeHarness h;
    EXPECT_FALSE(h.transport->is_open());
    h.transport->start();
    EXPECT_TRUE(h.transport->is_open());
    h.transport->close();
    EXPECT_FALSE(h.transport->is_open());
}

TEST(StdioTransport, IdempotentStartAndClose) {
    PipeHarness h;
    h.transport->start();
    h.transport->start();   // second start() is a no-op
    h.transport->close();
    h.transport->close();   // second close() is a no-op
    SUCCEED();
}

TEST(StdioTransport, OversizedFrameTriggersErrorAndClose) {
    int t2[2];
    int f2[2];
    ASSERT_EQ(::pipe(t2), 0);
    ASSERT_EQ(::pipe(f2), 0);

    mcp::StdioTransport::Options opts{};
    opts.read_fd         = t2[0];
    opts.write_fd        = f2[1];
    opts.owns_fds        = true;
    opts.max_frame_bytes = 16;
    auto transport = std::make_unique<mcp::StdioTransport>(opts);

    Sink sink;
    transport->on_error([&](std::error_code ec) { sink.on_error(ec); });
    transport->on_close([&]                      { sink.on_close(); });
    transport->start();

    std::string oversized(64, 'x');  // no newline; exceeds max_frame_bytes=16
    ssize_t n = ::write(t2[1], oversized.data(), oversized.size());
    ASSERT_EQ(static_cast<std::size_t>(n), oversized.size());

    {
        std::unique_lock<std::mutex> lk(sink.mu);
        sink.cv.wait_for(lk, 2s, [&] { return !sink.errors.empty() || sink.closed; });
    }
    ASSERT_FALSE(sink.errors.empty());
    EXPECT_EQ(sink.errors.front(), std::make_error_code(std::errc::message_size));

    transport->close();
    ::close(t2[1]);
    ::close(f2[0]);
}

}  // namespace
