// SPDX-License-Identifier: Apache-2.0
//
// End-to-end test: an in-process HttpServerHost paired with a Client
// driving it through HttpClientTransport. Proves the full Streamable
// HTTP path works for a non-streaming server: initialize handshake,
// session-id round-tripping, tools/list, and tools/call.

#if !defined(MCP_ENABLE_HTTP)
#error "MCP_ENABLE_HTTP must be defined for http_e2e_test"
#endif

#include "mcp/client.hpp"
#include "mcp/http_client_transport.hpp"
#include "mcp/http_server_host.hpp"
#include "mcp/log.hpp"
#include "mcp/protocol.hpp"

#include <httplib.h>

#include <cstdlib>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

struct EnableTraceLogging {
    EnableTraceLogging() {
        if (std::getenv("MCP_TRACE")) {
            mcp::set_log_level(mcp::LogLevel::trace);
        }
    }
};
EnableTraceLogging g_enable_trace;

TEST(HttpEndToEnd, InitializeAndToolCall) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "calc-http", .version = "1.0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .port = 0,  // OS-assigned
            .path = "/mcp",
        },
        [](mcp::Server& s) {
            s.tool("add",
                   json{{"type", "object"},
                        {"properties",
                         {{"a", {{"type", "number"}}},
                          {"b", {{"type", "number"}}}}},
                        {"required", json::array({"a", "b"})}},
                   [](const json& args) -> mcp::CallToolResult {
                       const double a = args.at("a").get<double>();
                       const double b = args.at("b").get<double>();
                       return {.content = { mcp::TextContent{
                           .text = std::to_string(a + b),
                       }}};
                   });
        },
    };
    host.start();
    EXPECT_GT(host.port(), 0);

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + std::to_string(host.port()) + "/mcp";
    topts.open_get_stream = false;  // server doesn't offer one in 3e/2

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::make_unique<mcp::HttpClientTransport>(topts));

    auto info = client.initialize().get();
    EXPECT_EQ(info.protocol_version, mcp::kLatestProtocolVersion);
    EXPECT_EQ(info.server_info.name, "calc-http");
    ASSERT_TRUE(info.capabilities.tools.has_value());

    auto tools = client.list_tools().get();
    ASSERT_EQ(tools.tools.size(), 1u);
    EXPECT_EQ(tools.tools[0].name, "add");

    auto out = client.call_tool("add",
                                json{{"a", 2.5}, {"b", 1.5}}).get();
    ASSERT_FALSE(out.is_error.value_or(false));
    ASSERT_EQ(out.content.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<mcp::TextContent>(out.content[0]));
    EXPECT_NE(std::get<mcp::TextContent>(out.content[0]).text.find("4."),
              std::string::npos);

    client.disconnect();
    host.stop();
}

TEST(HttpEndToEnd, OriginAllowlistRejectsCrossOrigin) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .allowed_origins = {"https://allowed.example"},
        },
        [](mcp::Server&) {},
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + std::to_string(host.port()) + "/mcp";
    topts.open_get_stream = false;
    topts.extra_headers["Origin"] = "https://evil.example";

    auto transport = std::make_unique<mcp::HttpClientTransport>(topts);
    std::promise<std::error_code> got_error;
    auto fut = got_error.get_future();
    transport->on_error([&](std::error_code ec) {
        try { got_error.set_value(ec); } catch (...) {}
    });
    transport->start();
    EXPECT_FALSE(transport->send(R"({"jsonrpc":"2.0","method":"x"})"));

    EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
    transport->close();
    host.stop();
}

TEST(HttpEndToEnd, ServerInitiatedSamplingThroughGetStream) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "agent", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
        },
        [](mcp::Server& s) {
            // Tool that itself asks the client to sample. Exercises
            // the full server-initiated → GET-stream → client →
            // back-through-POST round trip.
            s.tool("ask",
                   json{{"type", "object"},
                        {"properties", {{"q", {{"type", "string"}}}}}},
                   [&s](const json& args) -> mcp::CallToolResult {
                       const std::string q = args.value("q", "?");
                       auto resp = s.sample(mcp::CreateMessageRequestParams{
                           .messages = { mcp::SamplingMessage{
                               .role    = mcp::Role::user,
                               .content = { mcp::TextContent{.text = q} },
                           }},
                           .max_tokens = 100,
                       }).get();
                       return {.content = { mcp::TextContent{
                           .text = "got: " + std::get<mcp::TextContent>(
                                                resp.content.at(0)).text,
                       }}};
                   });
        },
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + std::to_string(host.port()) + "/mcp";
    topts.open_get_stream = true;  // required for sampling round-trip

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::make_unique<mcp::HttpClientTransport>(topts));
    client.set_sampling_handler(
        [](const mcp::CreateMessageRequestParams& req) {
            const std::string in =
                std::get<mcp::TextContent>(req.messages.at(0).content.at(0)).text;
            return mcp::CreateMessageResult{
                .role    = mcp::Role::assistant,
                .content = { mcp::TextContent{.text = "echo:" + in} },
                .model   = "test-model",
            };
        });
    (void)client.initialize().get();

    auto out = client.call_tool("ask", json{{"q", "hello"}}).get();
    ASSERT_FALSE(out.is_error.value_or(false));
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text,
              "got: echo:hello");

    client.disconnect();
    host.stop();
}

TEST(HttpEndToEnd, SessionIdReusedAcrossCalls) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
        },
        [](mcp::Server& s) {
            s.tool("echo", json{{"type", "object"}},
                   [](const json& args) -> mcp::CallToolResult {
                       (void)args;
                       return {.content = { mcp::TextContent{.text = "ok"} }};
                   });
        },
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + std::to_string(host.port()) + "/mcp";
    topts.open_get_stream = false;
    auto transport_ptr = new mcp::HttpClientTransport{topts};
    std::unique_ptr<mcp::HttpClientTransport> transport{transport_ptr};

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(transport));

    (void)client.initialize().get();
    const auto sid = transport_ptr->session_id();
    ASSERT_TRUE(sid.has_value());
    EXPECT_FALSE(sid->empty());

    EXPECT_EQ(host.active_sessions(), 1u);

    // Multiple call_tool requests should all reuse this session.
    for (int i = 0; i < 3; ++i) {
        auto out = client.call_tool("echo").get();
        EXPECT_FALSE(out.is_error.value_or(false));
    }
    EXPECT_EQ(transport_ptr->session_id(), sid);
    EXPECT_EQ(host.active_sessions(), 1u);

    client.disconnect();
    host.stop();
}

TEST(HttpEndToEnd, UnsupportedMcpProtocolVersionIs400) {
    // Spec MUST: a request with a present-but-unsupported
    // MCP-Protocol-Version header is rejected with 400.
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    auto res = cli.Post("/mcp",
        {{"Content-Type",         "application/json"},
         {"MCP-Protocol-Version", "1999-01-01"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})",
        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_NE(res->body.find("unsupported MCP-Protocol-Version"),
              std::string::npos);

    host.stop();
}

TEST(HttpEndToEnd, ClientSendsDeleteOnGracefulClose) {
    // Spec: a client that no longer needs a session SHOULD send an
    // HTTP DELETE on the MCP path. Verify our transport does it,
    // and that the server's session table reflects the removal.
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
        },
        [](mcp::Server& s) {
            s.tool("noop", json{{"type", "object"}},
                   [](const json&) -> mcp::CallToolResult {
                       return {.content = { mcp::TextContent{.text = "ok"} }};
                   });
        },
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url             = "http://127.0.0.1:" + std::to_string(host.port())
                           + "/mcp";
    topts.open_get_stream = false;

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::make_unique<mcp::HttpClientTransport>(topts));
    (void)client.initialize().get();
    EXPECT_EQ(host.active_sessions(), 1u);

    // Disconnect should drive the transport's close(), which fires
    // the spec-mandated DELETE before stopping the cpp-httplib
    // clients.
    client.disconnect();

    // Allow a brief window for the DELETE to land.
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (host.active_sessions() != 0u &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(host.active_sessions(), 0u);

    host.stop();
}

TEST(HttpEndToEnd, FixedPortIsHonored) {
    // Regression: start() used to call bind_to_any_port
    // unconditionally, silently ignoring Options::port. Grab an
    // OS-assigned port with a throwaway host, then demand it back
    // explicitly with a second one.
    int chosen = 0;
    {
        mcp::HttpServerHost probe{
            mcp::Implementation{.name = "probe", .version = "0"},
            mcp::HttpServerHost::Options{.host = "127.0.0.1", .path = "/mcp"},
            [](mcp::Server&) {},
        };
        probe.start();
        chosen = probe.port();
        probe.stop();
    }
    ASSERT_GT(chosen, 0);

    mcp::HttpServerHost host{
        mcp::Implementation{.name = "fixed", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .port = chosen,
            .path = "/mcp",
        },
        [](mcp::Server&) {},
    };
    host.start();
    EXPECT_EQ(host.port(), chosen);

    // And it actually answers there.
    httplib::Client cli{"http://127.0.0.1:" + std::to_string(chosen)};
    auto res = cli.Post("/mcp",
        {{"Content-Type", "application/json"},
         {"Accept",       "application/json, text/event-stream"}},
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
        R"({"protocolVersion":"2025-11-25","capabilities":{},)"
        R"("clientInfo":{"name":"t","version":"0"}}})",
        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    host.stop();
}

TEST(HttpEndToEnd, PostToTerminatedSessionIs404) {
    // Spec: a request carrying an Mcp-Session-Id the server no longer
    // recognizes MUST get 404 (the client's cue to re-initialize).
    // A session-less non-initialize POST stays a plain 400.
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{.host = "127.0.0.1", .path = "/mcp"},
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    httplib::Headers base{
        {"Content-Type", "application/json"},
        {"Accept",       "application/json, text/event-stream"},
    };

    auto init = cli.Post("/mcp", base,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
        R"({"protocolVersion":"2025-11-25","capabilities":{},)"
        R"("clientInfo":{"name":"t","version":"0"}}})",
        "application/json");
    ASSERT_TRUE(init);
    ASSERT_EQ(init->status, 200);
    const std::string sid = init->get_header_value("Mcp-Session-Id");
    ASSERT_FALSE(sid.empty());

    auto del = cli.Delete("/mcp", {{"Mcp-Session-Id", sid}});
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 204);

    httplib::Headers stale = base;
    stale.emplace("Mcp-Session-Id", sid);
    auto reuse = cli.Post("/mcp", stale,
        R"({"jsonrpc":"2.0","id":2,"method":"ping"})", "application/json");
    ASSERT_TRUE(reuse);
    EXPECT_EQ(reuse->status, 404);

    auto no_sid = cli.Post("/mcp", base,
        R"({"jsonrpc":"2.0","id":3,"method":"ping"})", "application/json");
    ASSERT_TRUE(no_sid);
    EXPECT_EQ(no_sid->status, 400);

    host.stop();
}

TEST(HttpEndToEnd, OnSessionClosedFiresOnceOnDelete) {
    // The pre-destruction hook must fire exactly once per session on the
    // client-DELETE teardown path, with the Server still alive. Track the
    // Server* seen so we can assert identity, and count invocations.
    std::mutex               seen_mu;
    std::vector<mcp::Server*> seen;
    std::atomic<int>          calls{0};

    mcp::HttpServerHost::Options opts;
    opts.host = "127.0.0.1";
    opts.path = "/mcp";
    opts.on_session_closed = [&](mcp::Server& s) {
        // The Server is alive here: an any-thread call must succeed.
        (void)s.client_capabilities();
        {
            std::lock_guard<std::mutex> lk(seen_mu);
            seen.push_back(&s);
        }
        calls.fetch_add(1, std::memory_order_relaxed);
    };

    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        opts,
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    httplib::Headers base{
        {"Content-Type", "application/json"},
        {"Accept",       "application/json, text/event-stream"},
    };
    auto init = cli.Post("/mcp", base,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
        R"({"protocolVersion":"2025-11-25","capabilities":{},)"
        R"("clientInfo":{"name":"t","version":"0"}}})",
        "application/json");
    ASSERT_TRUE(init);
    ASSERT_EQ(init->status, 200);
    const std::string sid = init->get_header_value("Mcp-Session-Id");
    ASSERT_FALSE(sid.empty());
    EXPECT_EQ(calls.load(), 0);  // not yet — session is live

    auto del = cli.Delete("/mcp", {{"Mcp-Session-Id", sid}});
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 204);

    // Fired exactly once, on the DELETE.
    EXPECT_EQ(calls.load(), 1);
    {
        std::lock_guard<std::mutex> lk(seen_mu);
        ASSERT_EQ(seen.size(), 1u);
        EXPECT_NE(seen[0], nullptr);
    }

    // stop() must not re-fire for an already-torn-down session.
    host.stop();
    EXPECT_EQ(calls.load(), 1);
}

TEST(HttpEndToEnd, OnSessionClosedFiresOnStop) {
    // stop() tears down every surviving session; the hook must fire once
    // for each, before any close/join/destroy.
    std::atomic<int> calls{0};

    mcp::HttpServerHost::Options opts;
    opts.host = "127.0.0.1";
    opts.path = "/mcp";
    opts.on_session_closed = [&](mcp::Server&) {
        calls.fetch_add(1, std::memory_order_relaxed);
    };

    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        opts,
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    httplib::Headers base{
        {"Content-Type", "application/json"},
        {"Accept",       "application/json, text/event-stream"},
    };
    // Two independent sessions (each a fresh initialize mints its own id).
    for (int i = 0; i < 2; ++i) {
        auto init = cli.Post("/mcp", base,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
            R"({"protocolVersion":"2025-11-25","capabilities":{},)"
            R"("clientInfo":{"name":"t","version":"0"}}})",
            "application/json");
        ASSERT_TRUE(init);
        ASSERT_EQ(init->status, 200);
    }
    EXPECT_EQ(host.active_sessions(), 2u);
    EXPECT_EQ(calls.load(), 0);

    host.stop();
    EXPECT_EQ(calls.load(), 2);

    // Idempotent: a second stop() fires nothing more.
    host.stop();
    EXPECT_EQ(calls.load(), 2);
}

TEST(HttpEndToEnd, ClientCapabilitiesCapturedAtInitialize) {
    // Before initialize: nullopt. After an initialize advertising known
    // capabilities: the accessor returns them, readable from the factory
    // thread via the session-closed hook (any-thread contract).
    mcp::Server fresh{ mcp::Implementation{.name = "fresh", .version = "0"} };
    EXPECT_FALSE(fresh.client_capabilities().has_value());

    std::promise<mcp::ClientCapabilities> captured;
    auto captured_fut = captured.get_future();

    mcp::HttpServerHost::Options opts;
    opts.host = "127.0.0.1";
    opts.path = "/mcp";
    opts.on_session_closed = [&](mcp::Server& s) {
        auto caps = s.client_capabilities();
        if (caps) {
            try { captured.set_value(*caps); } catch (...) {}
        }
    };

    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        opts,
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    httplib::Headers base{
        {"Content-Type", "application/json"},
        {"Accept",       "application/json, text/event-stream"},
    };
    // A client advertising sampling + elicitation.
    auto init = cli.Post("/mcp", base,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
        R"({"protocolVersion":"2025-11-25",)"
        R"("capabilities":{"sampling":{},"elicitation":{}},)"
        R"("clientInfo":{"name":"t","version":"0"}}})",
        "application/json");
    ASSERT_TRUE(init);
    ASSERT_EQ(init->status, 200);
    const std::string sid = init->get_header_value("Mcp-Session-Id");
    ASSERT_FALSE(sid.empty());

    auto del = cli.Delete("/mcp", {{"Mcp-Session-Id", sid}});
    ASSERT_TRUE(del);
    ASSERT_EQ(del->status, 204);

    ASSERT_EQ(captured_fut.wait_for(2s), std::future_status::ready);
    const mcp::ClientCapabilities caps = captured_fut.get();
    EXPECT_TRUE(caps.sampling.has_value());
    EXPECT_TRUE(caps.elicitation.has_value());
    EXPECT_FALSE(caps.roots.has_value());

    host.stop();
}

}  // namespace
