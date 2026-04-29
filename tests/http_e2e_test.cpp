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

#include <cstdlib>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <variant>

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

}  // namespace
