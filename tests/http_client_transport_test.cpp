// SPDX-License-Identifier: Apache-2.0
//
// Tests for HttpClientTransport. Each test stands up an in-process
// httplib::Server as a stand-in for a real MCP server and points the
// transport at it. We're testing the *transport* here, not the
// protocol — Session-level integration tests will follow once
// HttpServerHost lands.

#if !defined(MCP_ENABLE_HTTP)
// MCP_ENABLE_HTTP is required for these tests; the test target is
// only added to the build when it's on.
#error "MCP_ENABLE_HTTP must be defined for http_client_transport_test"
#endif

#include "mcp/http_client_transport.hpp"

#include <gtest/gtest.h>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// Tiny stand-in MCP server: an httplib::Server that the test fixture
// configures with handlers for POST/GET/DELETE on /mcp and runs on
// 127.0.0.1:<port>.
struct FakeServer {
    httplib::Server srv;
    int  port    = 0;
    std::thread thread;

    template <typename Configure>
    explicit FakeServer(Configure cfg) {
        cfg(srv);
        // Bind to an OS-assigned port on loopback.
        port = srv.bind_to_any_port("127.0.0.1");
        if (port < 0) throw std::runtime_error("FakeServer: bind failed");
        thread = std::thread([this] { srv.listen_after_bind(); });
        // Wait for the listener loop to be ready.
        while (!srv.is_running()) std::this_thread::sleep_for(1ms);
    }
    ~FakeServer() {
        srv.stop();
        if (thread.joinable()) thread.join();
    }
    [[nodiscard]] std::string url(const std::string& path = "/mcp") const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

// -------------------------------------------------------------------------

TEST(HttpClientTransport, PostSingleJsonResponseIsDelivered) {
    FakeServer fake([](httplib::Server& s) {
        s.Post("/mcp", [](const httplib::Request& req, httplib::Response& res) {
            EXPECT_EQ(req.body, R"({"hello":1})");
            res.set_content(R"({"ok":true})", "application/json");
        });
    });

    mcp::HttpClientTransport::Options opts;
    opts.url = fake.url();
    opts.open_get_stream = false;  // simpler for this test
    auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

    std::promise<std::string> got;
    auto fut = got.get_future();
    transport->on_message([&](std::string s) {
        try { got.set_value(std::move(s)); } catch (...) {}
    });
    transport->start();

    EXPECT_FALSE(transport->send(R"({"hello":1})"));
    ASSERT_EQ(fut.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(fut.get(), R"({"ok":true})");
    transport->close();
}

TEST(HttpClientTransport, PostSseResponseDeliversMultipleFrames) {
    FakeServer fake([](httplib::Server& s) {
        s.Post("/mcp", [](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider("text/event-stream",
                [](std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    constexpr std::string_view a = "data: {\"a\":1}\n\n";
                    constexpr std::string_view b = "data: {\"b\":2}\n\n";
                    sink.write(a.data(), a.size());
                    sink.write(b.data(), b.size());
                    sink.done();
                    return true;
                });
        });
    });

    mcp::HttpClientTransport::Options opts;
    opts.url = fake.url();
    opts.open_get_stream = false;
    auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> frames;
    transport->on_message([&](std::string s) {
        std::lock_guard<std::mutex> lk(mu);
        frames.push_back(std::move(s));
        cv.notify_all();
    });
    transport->start();
    EXPECT_FALSE(transport->send("{}"));

    {
        std::unique_lock<std::mutex> lk(mu);
        EXPECT_TRUE(cv.wait_for(lk, 2s, [&] { return frames.size() >= 2; }));
    }
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0], R"({"a":1})");
    EXPECT_EQ(frames[1], R"({"b":2})");
    transport->close();
}

TEST(HttpClientTransport, NotificationGets202AbsorbedSilently) {
    std::atomic<int> hits{0};
    FakeServer fake([&](httplib::Server& s) {
        s.Post("/mcp", [&](const httplib::Request&, httplib::Response& res) {
            ++hits;
            res.status = 202;  // accepted, no body
        });
    });

    mcp::HttpClientTransport::Options opts;
    opts.url = fake.url();
    opts.open_get_stream = false;
    auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

    std::atomic<int> messages{0};
    transport->on_message([&](std::string) { ++messages; });
    transport->start();
    EXPECT_FALSE(transport->send(R"({"jsonrpc":"2.0","method":"x"})"));
    std::this_thread::sleep_for(150ms);

    EXPECT_EQ(hits.load(), 1);
    EXPECT_EQ(messages.load(), 0);
    transport->close();
}

TEST(HttpClientTransport, McpSessionIdIsCapturedAndEchoed) {
    std::atomic<int> first_call{0};
    std::string captured_id_on_second_call;
    FakeServer fake([&](httplib::Server& s) {
        s.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
            const int n = ++first_call;
            if (n == 1) {
                res.set_header("Mcp-Session-Id", "deadbeef");
            } else {
                if (auto it = req.headers.find("Mcp-Session-Id");
                    it != req.headers.end()) {
                    captured_id_on_second_call = it->second;
                }
            }
            res.set_content("{}", "application/json");
        });
    });

    mcp::HttpClientTransport::Options opts;
    opts.url = fake.url();
    opts.open_get_stream = false;
    auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

    std::atomic<int> messages{0};
    transport->on_message([&](std::string) { ++messages; });
    transport->start();

    EXPECT_FALSE(transport->send("{}"));
    // wait for first response.
    while (messages.load() < 1) std::this_thread::sleep_for(5ms);
    EXPECT_EQ(transport->session_id(), std::optional<std::string>{"deadbeef"});

    EXPECT_FALSE(transport->send("{}"));
    while (messages.load() < 2) std::this_thread::sleep_for(5ms);
    EXPECT_EQ(captured_id_on_second_call, "deadbeef");

    transport->close();
}

TEST(HttpClientTransport, McpProtocolVersionHeaderIsSent) {
    std::string captured_pv;
    FakeServer fake([&](httplib::Server& s) {
        s.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
            if (auto it = req.headers.find("MCP-Protocol-Version");
                it != req.headers.end()) {
                captured_pv = it->second;
            }
            res.set_content("{}", "application/json");
        });
    });

    mcp::HttpClientTransport::Options opts;
    opts.url = fake.url();
    opts.protocol_version = "2025-11-25";
    opts.open_get_stream  = false;
    auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

    std::atomic<int> messages{0};
    transport->on_message([&](std::string) { ++messages; });
    transport->start();
    EXPECT_FALSE(transport->send("{}"));
    while (messages.load() < 1) std::this_thread::sleep_for(5ms);
    EXPECT_EQ(captured_pv, "2025-11-25");
    transport->close();
}

TEST(HttpClientTransport, GetStreamReceivesServerInitiatedFrames) {
    FakeServer fake([](httplib::Server& s) {
        s.Get("/mcp", [](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider("text/event-stream",
                [](std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    constexpr std::string_view event =
                        "data: {\"server-said\":\"hi\"}\n\n";
                    sink.write(event.data(), event.size());
                    sink.done();
                    return true;
                });
        });
        // POST is ignored for this test.
        s.Post("/mcp", [](const httplib::Request&, httplib::Response& res) {
            res.status = 202;
        });
    });

    mcp::HttpClientTransport::Options opts;
    opts.url = fake.url();
    opts.open_get_stream = true;
    auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

    std::promise<std::string> got;
    auto fut = got.get_future();
    transport->on_message([&](std::string s) {
        try { got.set_value(std::move(s)); } catch (...) {}
    });
    transport->start();

    EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(fut.get(), R"({"server-said":"hi"})");
    transport->close();
}

TEST(HttpClientTransport, SendBeforeStartFails) {
    mcp::HttpClientTransport::Options opts;
    opts.url = "http://127.0.0.1:1";
    opts.open_get_stream = false;
    auto transport = std::make_unique<mcp::HttpClientTransport>(opts);
    auto ec = transport->send("x");
    EXPECT_TRUE(ec);
    EXPECT_EQ(ec, std::make_error_code(std::errc::not_connected));
}

TEST(HttpClientTransport, IdempotentClose) {
    mcp::HttpClientTransport::Options opts;
    opts.url = "http://127.0.0.1:1";
    opts.open_get_stream = false;
    auto transport = std::make_unique<mcp::HttpClientTransport>(opts);
    transport->start();
    transport->close();
    transport->close();  // must be a no-op
    SUCCEED();
}

}  // namespace
