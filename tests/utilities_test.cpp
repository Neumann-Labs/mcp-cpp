// SPDX-License-Identifier: Apache-2.0
//
// Tests for ping, cancellation, progress, and protocol-level logging
// (the "in-band utilities" added in Phase 2).

#include "in_memory_transport.hpp"

#include "mcp/client.hpp"
#include "mcp/protocol.hpp"
#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

struct UtilServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread                  thread;

    template <typename Configure>
    UtilServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "util-test", .version = "1.0",
          })) {
        cfg(*server);
        thread = std::thread([s = server, t = std::move(t)]() mutable {
            s->run(std::move(t));
        });
    }
    ~UtilServerThread() {
        server->stop();
        if (thread.joinable()) thread.join();
    }
};

// -------------------------------------------------------------------------
// Round-trip tests
// -------------------------------------------------------------------------

TEST(Cancelled, RoundTrip) {
    mcp::CancelledNotificationParams p{
        .request_id = mcp::RequestId{42},
        .reason     = "user requested",
    };
    json j = p;
    EXPECT_EQ(j["requestId"], 42);
    EXPECT_EQ(j["reason"], "user requested");
    auto back = j.get<mcp::CancelledNotificationParams>();
    ASSERT_TRUE(back.request_id.has_value());
    EXPECT_EQ(back.request_id->as_integer(), 42);
}

TEST(Progress, RoundTrip) {
    mcp::ProgressNotificationParams p{
        .progress_token = mcp::ProgressToken{"tok"},
        .progress       = 0.5,
        .total          = 1.0,
        .message        = "halfway",
    };
    json j = p;
    EXPECT_EQ(j["progressToken"], "tok");
    EXPECT_EQ(j["progress"], 0.5);
    auto back = j.get<mcp::ProgressNotificationParams>();
    EXPECT_EQ(back.progress, 0.5);
    EXPECT_EQ(back.total,    1.0);
}

TEST(Logging, LevelStrings) {
    EXPECT_EQ(mcp::to_string(mcp::LoggingLevel::debug),     "debug");
    EXPECT_EQ(mcp::to_string(mcp::LoggingLevel::emergency), "emergency");
    json j = mcp::LoggingLevel::warning;
    EXPECT_EQ(j, "warning");
    auto back = j.get<mcp::LoggingLevel>();
    EXPECT_EQ(back, mcp::LoggingLevel::warning);
}

TEST(Logging, MessageNotificationRoundTrip) {
    mcp::LoggingMessageNotificationParams p{
        .level  = mcp::LoggingLevel::info,
        .logger = "scheduler",
        .data   = json{{"event", "tick"}},
    };
    json j = p;
    EXPECT_EQ(j["level"],  "info");
    EXPECT_EQ(j["logger"], "scheduler");
    EXPECT_EQ(j["data"]["event"], "tick");
    auto back = j.get<mcp::LoggingMessageNotificationParams>();
    EXPECT_EQ(back.level, mcp::LoggingLevel::info);
}

// -------------------------------------------------------------------------
// Ping
// -------------------------------------------------------------------------

TEST(Ping, ClientPingResolves) {
    auto p = mcp::test::make_in_memory_pair();
    UtilServerThread srv(std::move(p.b), [](mcp::Server&) {});

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    EXPECT_NO_THROW(client.ping().get());

    client.disconnect();
}

// -------------------------------------------------------------------------
// Logging
// -------------------------------------------------------------------------

TEST(LoggingIntegration, ServerLogReachesClient) {
    auto p = mcp::test::make_in_memory_pair();
    UtilServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_logging(mcp::LoggingLevel::debug);
        s.tool("emit_log",
               json{{"type", "object"}},
               [&s](const json&) {
                   (void)s.log(mcp::LoggingLevel::warning, json{{"msg", "hi"}}, "logger-1");
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{.text = "ok"} },
                   };
               });
    });

    std::promise<mcp::LoggingMessageNotificationParams> got;
    auto fut = got.get_future();

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_log_message_handler(
        [&](const mcp::LoggingMessageNotificationParams& params) {
            try { got.set_value(params); } catch (...) {}
        });
    auto info = client.initialize().get();
    ASSERT_TRUE(info.capabilities.logging.has_value());

    auto res = client.call_tool("emit_log").get();
    ASSERT_FALSE(res.is_error.value_or(false));

    auto received = fut.get();
    EXPECT_EQ(received.level, mcp::LoggingLevel::warning);
    EXPECT_EQ(received.logger, "logger-1");
    EXPECT_EQ(received.data["msg"], "hi");

    client.disconnect();
}

TEST(LoggingIntegration, SetLevelFiltersServerEmissions) {
    auto p = mcp::test::make_in_memory_pair();
    UtilServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_logging(mcp::LoggingLevel::debug);
        s.tool("emit_debug",
               json{{"type", "object"}},
               [&s](const json&) {
                   (void)s.log(mcp::LoggingLevel::debug, "should-be-filtered");
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{.text = "ok"} },
                   };
               });
    });

    std::mutex mu;
    int received_count = 0;

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_log_message_handler([&](const mcp::LoggingMessageNotificationParams&) {
        std::lock_guard<std::mutex> lk(mu);
        ++received_count;
    });
    (void)client.initialize().get();

    // Raise the server's level above debug, so debug emissions drop.
    (void)client.set_log_level(mcp::LoggingLevel::error).get();
    (void)client.call_tool("emit_debug").get();
    std::this_thread::sleep_for(50ms);

    {
        std::lock_guard<std::mutex> lk(mu);
        EXPECT_EQ(received_count, 0);
    }

    client.disconnect();
}

// -------------------------------------------------------------------------
// Progress + cancellation (limited Phase 2 scope)
// -------------------------------------------------------------------------

TEST(ProgressIntegration, ServerProgressReachesClient) {
    auto p = mcp::test::make_in_memory_pair();
    UtilServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("loud_op",
               json{{"type", "object"}},
               [&s](const json&) {
                   s.report_progress(mcp::ProgressToken{"job-1"}, 0.25);
                   s.report_progress(mcp::ProgressToken{"job-1"}, 0.75, 1.0, "almost");
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{.text = "done"} },
                   };
               });
    });

    std::mutex mu;
    std::vector<double> progresses;

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_progress_handler([&](const mcp::ProgressNotificationParams& pn) {
        std::lock_guard<std::mutex> lk(mu);
        progresses.push_back(pn.progress);
    });
    (void)client.initialize().get();

    (void)client.call_tool("loud_op").get();
    std::this_thread::sleep_for(50ms);

    std::lock_guard<std::mutex> lk(mu);
    ASSERT_GE(progresses.size(), 2u);
    EXPECT_DOUBLE_EQ(progresses[0], 0.25);
    EXPECT_DOUBLE_EQ(progresses[1], 0.75);

    client.disconnect();
}

TEST(CancellationIntegration, ClientCanSendCancelledNotification) {
    auto p = mcp::test::make_in_memory_pair();
    std::promise<mcp::CancelledNotificationParams> got;
    auto fut = got.get_future();

    auto pair = mcp::test::make_in_memory_pair();
    auto client_session = std::make_unique<mcp::Session>(std::move(pair.a));
    auto server_session = std::make_unique<mcp::Session>(std::move(pair.b));
    server_session->set_notification_handler(
        std::string{mcp::method_notifications_cancelled},
        [&](const json& params) {
            try { got.set_value(params.get<mcp::CancelledNotificationParams>()); }
            catch (...) {}
        });
    client_session->start();
    server_session->start();

    EXPECT_FALSE(client_session->send_notification(
        std::string{mcp::method_notifications_cancelled},
        json(mcp::CancelledNotificationParams{
            .request_id = mcp::RequestId{7},
            .reason     = "stopped",
        })));

    auto received = fut.get();
    ASSERT_TRUE(received.request_id.has_value());
    EXPECT_EQ(received.request_id->as_integer(), 7);

    client_session->close();
    server_session->close();
}

}  // namespace
