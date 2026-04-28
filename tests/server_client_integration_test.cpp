// SPDX-License-Identifier: Apache-2.0
//
// End-to-end tests using Server + Client over an in-memory transport pair.
// These exercise the full path: Client → Session → Transport → Session →
// Server → handler → reverse path back to the Client's future.

#include "in_memory_transport.hpp"

#include "mcp/client.hpp"
#include "mcp/error.hpp"
#include "mcp/protocol.hpp"
#include "mcp/server.hpp"
#include "mcp/transport.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

// Run a Server in a background thread with the supplied transport, and
// return a handle that joins on destruction. Lets the test drive the
// Client side from the foreground.
struct ServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread                  thread;

    template <typename Configure>
    ServerThread(mcp::Implementation info,
                 std::unique_ptr<mcp::Transport> transport,
                 Configure configure)
        : server(std::make_shared<mcp::Server>(std::move(info))) {
        configure(*server);
        thread = std::thread([s = server, t = std::move(transport)]() mutable {
            s->run(std::move(t));
        });
    }

    ~ServerThread() {
        server->stop();
        if (thread.joinable()) thread.join();
    }
};

// Build the canonical "calculator" server with two tools.
std::unique_ptr<ServerThread>
calc_server(std::unique_ptr<mcp::Transport> transport) {
    return std::make_unique<ServerThread>(
        mcp::Implementation{
            .name    = "calc",
            .version = "1.0.0",
        },
        std::move(transport),
        [](mcp::Server& s) {
            s.set_instructions("A tiny arithmetic server.");
            s.tool("add",
                json{{"type", "object"},
                     {"properties", {{"a", {{"type", "number"}}},
                                     {"b", {{"type", "number"}}}}},
                     {"required", json::array({"a", "b"})}},
                [](const json& args) -> mcp::CallToolResult {
                    const double a = args.at("a").get<double>();
                    const double b = args.at("b").get<double>();
                    return mcp::CallToolResult{
                        .content = { mcp::TextContent{
                            .text = std::to_string(a + b),
                        }},
                    };
                });
            s.tool("divide",
                json{{"type", "object"},
                     {"properties", {{"a", {{"type", "number"}}},
                                     {"b", {{"type", "number"}}}}},
                     {"required", json::array({"a", "b"})}},
                [](const json& args) -> mcp::CallToolResult {
                    const double b = args.at("b").get<double>();
                    if (b == 0.0) {
                        // Reportable error: visible to the LLM via isError.
                        return mcp::CallToolResult{
                            .content = { mcp::TextContent{
                                .text = "division by zero",
                            }},
                            .is_error = true,
                        };
                    }
                    const double a = args.at("a").get<double>();
                    return mcp::CallToolResult{
                        .content = { mcp::TextContent{
                            .text = std::to_string(a / b),
                        }},
                    };
                });
        });
}

// -------------------------------------------------------------------------

TEST(Integration, InitializeReturnsExpectedShape) {
    auto p = mcp::test::make_in_memory_pair();
    auto srv = calc_server(std::move(p.b));

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0.1.0"} };
    client.connect(std::move(p.a));

    auto info = client.initialize().get();
    EXPECT_EQ(info.protocol_version, mcp::kLatestProtocolVersion);
    EXPECT_EQ(info.server_info.name, "calc");
    ASSERT_TRUE(info.capabilities.tools.has_value());
    ASSERT_TRUE(info.instructions.has_value());
    EXPECT_EQ(*info.instructions, "A tiny arithmetic server.");

    client.disconnect();
}

TEST(Integration, ListToolsReturnsRegisteredTools) {
    auto p = mcp::test::make_in_memory_pair();
    auto srv = calc_server(std::move(p.b));

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0.1.0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto list = client.list_tools().get();
    ASSERT_EQ(list.tools.size(), 2u);
    std::vector<std::string> names;
    for (const auto& t : list.tools) names.push_back(t.name);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"add", "divide"}));

    client.disconnect();
}

TEST(Integration, CallToolReturnsTextContent) {
    auto p = mcp::test::make_in_memory_pair();
    auto srv = calc_server(std::move(p.b));

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0.1.0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto out = client.call_tool("add", json{{"a", 2.0}, {"b", 3.5}}).get();
    ASSERT_EQ(out.content.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<mcp::TextContent>(out.content[0]));
    const auto& text = std::get<mcp::TextContent>(out.content[0]).text;
    EXPECT_NE(text.find("5.5"), std::string::npos);
    EXPECT_FALSE(out.is_error.value_or(false));

    client.disconnect();
}

TEST(Integration, ToolErrorIsReportedViaIsError) {
    auto p = mcp::test::make_in_memory_pair();
    auto srv = calc_server(std::move(p.b));

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0.1.0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto out = client.call_tool("divide", json{{"a", 1.0}, {"b", 0.0}}).get();
    ASSERT_TRUE(out.is_error.has_value());
    EXPECT_TRUE(*out.is_error);
    ASSERT_EQ(out.content.size(), 1u);
    EXPECT_NE(std::get<mcp::TextContent>(out.content[0]).text.find("division by zero"),
              std::string::npos);

    client.disconnect();
}

TEST(Integration, MissingToolMapsToMethodNotFound) {
    auto p = mcp::test::make_in_memory_pair();
    auto srv = calc_server(std::move(p.b));

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0.1.0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto fut = client.call_tool("does-not-exist");
    try {
        (void)fut.get();
        FAIL() << "expected Error";
    } catch (const mcp::Error& e) {
        EXPECT_EQ(e.code(), mcp::error_code::method_not_found);
    }

    client.disconnect();
}

TEST(Integration, RequestsBeforeInitializeAreRejected) {
    auto p = mcp::test::make_in_memory_pair();
    auto srv = calc_server(std::move(p.b));

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0.1.0"} };
    client.connect(std::move(p.a));

    auto fut = client.list_tools();
    try {
        (void)fut.get();
        FAIL() << "expected Error before initialize";
    } catch (const mcp::Error& e) {
        EXPECT_EQ(e.code(), mcp::error_code::invalid_request);
    }

    client.disconnect();
}

TEST(Integration, ConcurrentToolCallsAllResolve) {
    auto p = mcp::test::make_in_memory_pair();
    auto srv = calc_server(std::move(p.b));

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0.1.0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    constexpr int N = 50;
    std::vector<std::future<mcp::CallToolResult>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        futs.push_back(client.call_tool("add", json{{"a", i}, {"b", 1}}));
    }
    for (int i = 0; i < N; ++i) {
        auto r = futs[i].get();
        EXPECT_FALSE(r.is_error.value_or(false));
    }

    client.disconnect();
}

TEST(Integration, ServerInfoCachedOnInitialize) {
    auto p = mcp::test::make_in_memory_pair();
    auto srv = calc_server(std::move(p.b));

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0.1.0"} };
    client.connect(std::move(p.a));
    EXPECT_FALSE(client.server().has_value());
    (void)client.initialize().get();
    auto cached = client.server();
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached->server_info.name, "calc");
    client.disconnect();
    EXPECT_FALSE(client.server().has_value());
}

}  // namespace
