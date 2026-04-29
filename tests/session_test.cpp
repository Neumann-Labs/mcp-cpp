// SPDX-License-Identifier: Apache-2.0
//
// Tests for the Session dispatcher. Each test stands up a paired
// in-memory transport, drives one Session as the "client" and either
// the other Session or a hand-rolled handler as the "server", and
// asserts the wire-level effect.

#include "in_memory_transport.hpp"

#include "mcp/error.hpp"
#include "mcp/protocol.hpp"
#include "mcp/session.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

// Helper that builds two Sessions back-to-back via in-memory pair.
struct Pair {
    std::unique_ptr<mcp::Session> a;
    std::unique_ptr<mcp::Session> b;
};

Pair make_session_pair(mcp::Session::Options opts = {}) {
    auto p = mcp::test::make_in_memory_pair();
    Pair sp{
        std::make_unique<mcp::Session>(std::move(p.a), opts),
        std::make_unique<mcp::Session>(std::move(p.b), opts),
    };
    sp.a->start();
    sp.b->start();
    return sp;
}

// -------------------------------------------------------------------------

TEST(Session, RequestRoundTripThroughHandler) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("echo", [](const json& params) -> json {
        return json{{"echoed", params}};
    });

    auto fut = sp.a->send_request("echo", json{{"hi", 1}}, 2s);
    auto result = fut.get();
    EXPECT_EQ(result["echoed"]["hi"], 1);

    sp.a->close();
    sp.b->close();
}

TEST(Session, NotificationDeliveredToHandler) {
    auto sp = make_session_pair();

    std::promise<json> got;
    auto fut = got.get_future();

    sp.b->set_notification_handler("ping", [&](const json& params) {
        got.set_value(params);
    });

    EXPECT_FALSE(sp.a->send_notification("ping", json{{"k", "v"}}));

    auto p = fut.get();
    EXPECT_EQ(p["k"], "v");

    sp.a->close();
    sp.b->close();
}

TEST(Session, MissingHandlerYieldsMethodNotFound) {
    auto sp = make_session_pair();
    auto fut = sp.a->send_request("nope", nullptr, 2s);
    EXPECT_THROW({
        try { (void)fut.get(); }
        catch (const mcp::Error& e) {
            EXPECT_EQ(e.code(), mcp::error_code::method_not_found);
            throw;
        }
    }, mcp::Error);

    sp.a->close();
    sp.b->close();
}

TEST(Session, HandlerThrowingMcpErrorPropagates) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("explode", [](const json&) -> json {
        throw mcp::Error{mcp::error_code::invalid_params, "bad", json{{"why", "missing"}}};
    });

    auto fut = sp.a->send_request("explode", nullptr, 2s);
    try {
        (void)fut.get();
        FAIL() << "expected Error";
    } catch (const mcp::Error& e) {
        EXPECT_EQ(e.code(), mcp::error_code::invalid_params);
        EXPECT_EQ(e.message(), "bad");
        EXPECT_EQ(e.data()["why"], "missing");
    }

    sp.a->close();
    sp.b->close();
}

TEST(Session, HandlerThrowingStdExceptionMapsToInternal) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("explode", [](const json&) -> json {
        throw std::runtime_error{"boom"};
    });

    auto fut = sp.a->send_request("explode", nullptr, 2s);
    try {
        (void)fut.get();
        FAIL() << "expected Error";
    } catch (const mcp::Error& e) {
        EXPECT_EQ(e.code(), mcp::error_code::internal_error);
    }

    sp.a->close();
    sp.b->close();
}

TEST(Session, FallbackHandlerCatchesUnknownMethods) {
    auto sp = make_session_pair();
    sp.b->set_fallback_request_handler([](const json&) -> json {
        return json{{"caught_by", "fallback"}};
    });

    auto fut = sp.a->send_request("anything", nullptr, 2s);
    auto r = fut.get();
    EXPECT_EQ(r["caught_by"], "fallback");

    sp.a->close();
    sp.b->close();
}

TEST(Session, ManyConcurrentRequestsAllResolveCorrectly) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("double", [](const json& p) -> json {
        return json{{"r", p["x"].get<int>() * 2}};
    });

    constexpr int N = 200;
    std::vector<std::future<json>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        futs.push_back(sp.a->send_request("double", json{{"x", i}}, 5s));
    }
    for (int i = 0; i < N; ++i) {
        auto r = futs[i].get();
        EXPECT_EQ(r["r"], i * 2);
    }

    sp.a->close();
    sp.b->close();
}

TEST(Session, RequestTimeoutFiresErrorOnFuture) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("slow", [](const json&) -> json {
        std::this_thread::sleep_for(2s);
        return {};
    });

    auto fut = sp.a->send_request("slow", nullptr, 100ms);
    try {
        (void)fut.get();
        FAIL() << "expected timeout error";
    } catch (const mcp::Error& e) {
        EXPECT_EQ(e.code(), mcp::error_code::internal_error);
        EXPECT_NE(e.message().find("timed out"), std::string::npos);
    }

    sp.a->close();
    sp.b->close();
}

TEST(Session, ClosingCancelsPendingRequests) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("hang", [](const json&) -> json {
        std::this_thread::sleep_for(300ms);
        return {};
    });

    auto fut = sp.a->send_request("hang", nullptr, 30s);
    sp.a->close();
    try {
        (void)fut.get();
        FAIL() << "expected error after close";
    } catch (const mcp::Error&) {
        SUCCEED();
    }

    sp.b->close();
}

TEST(Session, IdsForDifferentRequestsAreDistinct) {
    auto sp = make_session_pair();
    // Two requests are dispatched on detached workers, so the handler
    // can run on either thread first — the bookkeeping must be
    // thread-safe.
    std::mutex               ids_mu;
    std::vector<std::string> ids;
    sp.b->set_request_handler("tag", [&](const json& p) -> json {
        std::lock_guard<std::mutex> lk(ids_mu);
        ids.push_back(p["id"].get<std::string>());
        return {};
    });
    auto f1 = sp.a->send_request("tag", json{{"id", "x"}}, 2s);
    auto f2 = sp.a->send_request("tag", json{{"id", "y"}}, 2s);
    f1.get();
    f2.get();
    {
        std::lock_guard<std::mutex> lk(ids_mu);
        EXPECT_EQ(ids.size(), 2u);
    }

    sp.a->close();
    sp.b->close();
}

TEST(Session, IsOpenReflectsLifecycle) {
    auto sp = make_session_pair();
    EXPECT_TRUE(sp.a->is_open());
    sp.a->close();
    EXPECT_FALSE(sp.a->is_open());
    sp.b->close();
}

TEST(Session, DoubleCloseDoesNotLeaveJoinableThread) {
    // Regression: close() short-circuiting once closed_ was true left the
    // timeout thread joinable, so ~Session called std::terminate from
    // ~thread. Both close() callers must run to completion.
    auto sp = make_session_pair();
    sp.a->close();
    sp.a->close();           // explicit double close
    SUCCEED();               // arriving here without terminate is the test
    sp.b->close();
}

TEST(Session, OnClosedHookFiresWhenPeerCloses) {
    auto p = mcp::test::make_in_memory_pair();
    auto a = std::make_unique<mcp::Session>(std::move(p.a));
    auto b = std::make_unique<mcp::Session>(std::move(p.b));

    std::promise<void> got_close;
    a->set_on_closed([&] { got_close.set_value(); });
    a->start();
    b->start();

    b->close();
    EXPECT_EQ(got_close.get_future().wait_for(2s), std::future_status::ready);

    a->close();
}

TEST(Session, MalformedFrameIsDropped) {
    auto p = mcp::test::make_in_memory_pair();
    auto a = std::make_unique<mcp::Session>(std::move(p.a));
    // Sneak the raw end onto the b-side so we can poke garbage into a's
    // dispatcher: we send a frame from b that isn't valid JSON.
    auto& b_transport = *p.b;
    b_transport.start();
    a->start();

    auto invalid = "{not json";
    EXPECT_FALSE(b_transport.send(invalid));

    // Subsequent valid frames still get through.
    std::promise<json> got;
    auto f = got.get_future();
    a->set_notification_handler("hi", [&](const json& params) {
        got.set_value(params);
    });
    EXPECT_FALSE(b_transport.send(R"({"jsonrpc":"2.0","method":"hi","params":{"k":1}})"));
    EXPECT_EQ(f.get()["k"], 1);

    a->close();
    b_transport.close();
}

}  // namespace
