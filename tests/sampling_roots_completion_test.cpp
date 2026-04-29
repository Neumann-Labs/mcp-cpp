// SPDX-License-Identifier: Apache-2.0
//
// Phase 3 integration tests: server-initiated sampling, roots,
// completion. The interesting one is sampling — it exercises the
// session's worker dispatch (a tool handler in the server thread
// must be able to await a request it issued back to the client).

#include "in_memory_transport.hpp"

#include "mcp/client.hpp"
#include "mcp/protocol.hpp"
#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

struct ServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread                  thread;

    template <typename Configure>
    ServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "phase3", .version = "1.0",
          })) {
        cfg(*server);
        thread = std::thread([s = server, t = std::move(t)]() mutable {
            s->run(std::move(t));
        });
    }
    ~ServerThread() {
        server->stop();
        if (thread.joinable()) thread.join();
    }
};

// -------------------------------------------------------------------------
// Round-trip type tests
// -------------------------------------------------------------------------

TEST(Sampling, CreateMessageRoundTrip) {
    mcp::CreateMessageRequestParams p{
        .messages = { mcp::SamplingMessage{
            .role    = mcp::Role::user,
            .content = { mcp::TextContent{.text = "hi"} },
        }},
        .model_preferences = mcp::ModelPreferences{
            .hints                 = std::vector<mcp::ModelHint>{{.name = "claude-3-5-sonnet"}},
            .intelligence_priority = 0.9,
        },
        .max_tokens = 256,
    };
    json j = p;
    EXPECT_EQ(j["maxTokens"], 256);
    EXPECT_EQ(j["modelPreferences"]["hints"][0]["name"], "claude-3-5-sonnet");
    auto back = j.get<mcp::CreateMessageRequestParams>();
    EXPECT_EQ(back.max_tokens, 256);
    ASSERT_EQ(back.messages.size(), 1u);
}

TEST(Sampling, IncludeContextRoundTrip) {
    mcp::CreateMessageRequestParams p{
        .messages         = { mcp::SamplingMessage{
            .role    = mcp::Role::user,
            .content = { mcp::TextContent{.text = "x"} },
        }},
        .include_context  = mcp::IncludeContext::all_servers,
        .max_tokens       = 1,
    };
    json j = p;
    EXPECT_EQ(j["includeContext"], "allServers");
    auto back = j.get<mcp::CreateMessageRequestParams>();
    EXPECT_EQ(back.include_context, mcp::IncludeContext::all_servers);
}

TEST(Sampling, CreateMessageResultRoundTrip) {
    mcp::CreateMessageResult r{
        .role        = mcp::Role::assistant,
        .content     = { mcp::TextContent{.text = "answer"} },
        .model       = "claude-3-5-sonnet-20241022",
        .stop_reason = "endTurn",
    };
    json j = r;
    EXPECT_EQ(j["role"],       "assistant");
    EXPECT_EQ(j["model"],      "claude-3-5-sonnet-20241022");
    EXPECT_EQ(j["stopReason"], "endTurn");
    auto back = j.get<mcp::CreateMessageResult>();
    EXPECT_EQ(back.model, r.model);
}

TEST(Roots, RoundTrip) {
    mcp::ListRootsResult r{
        .roots = {
            mcp::Root{.uri = "file:///workspace", .name = "workspace"},
            mcp::Root{.uri = "https://example.com/data"},
        },
    };
    json j = r;
    EXPECT_EQ(j["roots"][0]["name"], "workspace");
    auto back = j.get<mcp::ListRootsResult>();
    EXPECT_EQ(back.roots.size(), 2u);
}

TEST(Completion, ReferenceVariantRoundTrip) {
    mcp::CompletionReference ref = mcp::PromptReference{.name = "p1"};
    json j = ref;
    EXPECT_EQ(j["type"], "ref/prompt");
    auto back = j.get<mcp::CompletionReference>();
    ASSERT_TRUE(std::holds_alternative<mcp::PromptReference>(back));
    EXPECT_EQ(std::get<mcp::PromptReference>(back).name, "p1");

    ref = mcp::ResourceTemplateReference{.uri_template = "file:///{x}"};
    j = ref;
    EXPECT_EQ(j["type"], "ref/resource");
    EXPECT_EQ(j["uri"],  "file:///{x}");
}

TEST(Completion, RequestRoundTrip) {
    mcp::CompleteRequestParams p{
        .reference         = mcp::PromptReference{.name = "greet"},
        .argument          = mcp::CompleteArgument{.name = "name", .value = "Al"},
        .context_arguments = std::unordered_map<std::string, std::string>{
            {"locale", "en-US"},
        },
    };
    json j = p;
    EXPECT_EQ(j["argument"]["name"],  "name");
    EXPECT_EQ(j["argument"]["value"], "Al");
    EXPECT_EQ(j["context"]["arguments"]["locale"], "en-US");
    auto back = j.get<mcp::CompleteRequestParams>();
    EXPECT_EQ(back.argument.value, "Al");
    ASSERT_TRUE(back.context_arguments.has_value());
}

// -------------------------------------------------------------------------
// Sampling integration: server tool calls server.sample()
// -------------------------------------------------------------------------

TEST(SamplingIntegration, ToolHandlerCanRoundTripThroughClient) {
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("ask",
               json{{"type", "object"},
                    {"properties", {{"q", {{"type", "string"}}}}}},
               [&s](const json& args) -> mcp::CallToolResult {
                   const std::string q = args.value("q", "?");
                   mcp::CreateMessageRequestParams params{
                       .messages = { mcp::SamplingMessage{
                           .role    = mcp::Role::user,
                           .content = { mcp::TextContent{.text = q} },
                       }},
                       .max_tokens = 100,
                   };
                   auto fut = s.sample(std::move(params));
                   auto res = fut.get();
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{
                           .text = "got: " + std::get<mcp::TextContent>(res.content.at(0)).text,
                       }},
                   };
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_sampling_handler(
        [](const mcp::CreateMessageRequestParams& req) -> mcp::CreateMessageResult {
            const std::string in =
                std::get<mcp::TextContent>(req.messages.at(0).content.at(0)).text;
            return mcp::CreateMessageResult{
                .role    = mcp::Role::assistant,
                .content = { mcp::TextContent{.text = "echo:" + in} },
                .model   = "test-model",
            };
        });

    auto info = client.initialize().get();
    (void)info;

    auto out = client.call_tool("ask", json{{"q", "hello"}}).get();
    ASSERT_FALSE(out.is_error.value_or(false));
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "got: echo:hello");

    client.disconnect();
}

TEST(SamplingIntegration, MissingHandlerYieldsError) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("ask", json{{"type", "object"}},
               [&s](const json&) -> mcp::CallToolResult {
                   try {
                       (void)s.sample(mcp::CreateMessageRequestParams{
                           .messages = { mcp::SamplingMessage{
                               .role    = mcp::Role::user,
                               .content = { mcp::TextContent{.text = "x"} },
                           }},
                           .max_tokens = 1,
                       }).get();
                   } catch (const mcp::Error&) {
                       return mcp::CallToolResult{
                           .content = { mcp::TextContent{.text = "no-sampling"} },
                           .is_error = true,
                       };
                   }
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{.text = "unexpected-success"} },
                   };
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));  // no sampling handler
    (void)client.initialize().get();

    auto out = client.call_tool("ask").get();
    ASSERT_TRUE(out.is_error.value_or(false));

    client.disconnect();
}

// -------------------------------------------------------------------------
// Roots integration
// -------------------------------------------------------------------------

TEST(RootsIntegration, ServerCanQueryClientRoots) {
    auto p = mcp::test::make_in_memory_pair();

    std::promise<mcp::ListRootsResult> roots_received;
    auto roots_future = roots_received.get_future();

    ServerThread srv(std::move(p.b), [&](mcp::Server& s) {
        s.tool("read_roots", json{{"type", "object"}},
               [&s, &roots_received](const json&) -> mcp::CallToolResult {
                   auto roots = s.list_roots().get();
                   try { roots_received.set_value(roots); } catch (...) {}
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{
                           .text = std::to_string(roots.roots.size())
                       }},
                   };
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_roots_list_handler([]() -> mcp::ListRootsResult {
        return {
            .roots = {
                mcp::Root{.uri = "file:///a", .name = "A"},
                mcp::Root{.uri = "file:///b"},
            },
        };
    });
    auto info = client.initialize().get();
    ASSERT_TRUE(info.capabilities.tools.has_value());

    auto out = client.call_tool("read_roots").get();
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "2");

    auto roots = roots_future.get();
    EXPECT_EQ(roots.roots.size(), 2u);
    client.disconnect();
}

// -------------------------------------------------------------------------
// Completion integration
// -------------------------------------------------------------------------

TEST(CompletionIntegration, ServerProvidesCompletions) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_completion(
            [](const mcp::CompleteRequestParams& req) -> mcp::CompletionValues {
                if (std::holds_alternative<mcp::PromptReference>(req.reference)) {
                    return mcp::CompletionValues{
                        .values = { req.argument.value + "_a",
                                    req.argument.value + "_b" },
                    };
                }
                return {};
            });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    auto info = client.initialize().get();
    ASSERT_TRUE(info.capabilities.completions.has_value());

    auto out = client.complete(
        mcp::PromptReference{.name = "greet"},
        mcp::CompleteArgument{.name = "name", .value = "Al"}
    ).get();
    EXPECT_EQ(out.completion.values.size(), 2u);
    EXPECT_EQ(out.completion.values[0], "Al_a");

    client.disconnect();
}

}  // namespace
