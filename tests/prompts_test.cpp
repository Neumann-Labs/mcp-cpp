// SPDX-License-Identifier: Apache-2.0
//
// Tests for the prompts surface added in Phase 2: list_prompts,
// get_prompt, plus the protocol types PromptArgument, Prompt,
// PromptMessage.

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
#include <unordered_map>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

// -------------------------------------------------------------------------
// Round-trip type tests
// -------------------------------------------------------------------------

TEST(Prompts, PromptArgumentRoundTrip) {
    mcp::PromptArgument a{
        .name        = "topic",
        .title       = "Topic",
        .description = "What to write about",
        .required    = true,
    };
    json j = a;
    EXPECT_EQ(j["name"],     "topic");
    EXPECT_EQ(j["required"], true);
    auto back = j.get<mcp::PromptArgument>();
    EXPECT_EQ(back.name,     a.name);
    EXPECT_EQ(back.required, a.required);
}

TEST(Prompts, PromptRoundTripWithArguments) {
    mcp::Prompt p{
        .name        = "summarize",
        .description = "Summarize text",
        .arguments   = std::vector<mcp::PromptArgument>{
            {.name = "text", .required = true},
            {.name = "max_words"},
        },
    };
    json j = p;
    ASSERT_TRUE(j.contains("arguments"));
    EXPECT_EQ(j["arguments"].size(), 2u);
    auto back = j.get<mcp::Prompt>();
    ASSERT_TRUE(back.arguments.has_value());
    EXPECT_EQ(back.arguments->size(), 2u);
    EXPECT_EQ((*back.arguments)[0].name, "text");
}

TEST(Prompts, PromptMessageRoundTrip) {
    mcp::PromptMessage m{
        .role    = mcp::Role::user,
        .content = mcp::TextContent{.text = "hello"},
    };
    json j = m;
    EXPECT_EQ(j["role"], "user");
    EXPECT_EQ(j["content"]["type"], "text");
    auto back = j.get<mcp::PromptMessage>();
    EXPECT_EQ(back.role, mcp::Role::user);
    ASSERT_TRUE(std::holds_alternative<mcp::TextContent>(back.content));
}

TEST(Prompts, GetPromptResultRoundTrip) {
    mcp::GetPromptResult r{
        .description = "A summary prompt",
        .messages    = {
            mcp::PromptMessage{.role = mcp::Role::user,
                               .content = mcp::TextContent{.text = "Summarize:"}},
            mcp::PromptMessage{.role = mcp::Role::assistant,
                               .content = mcp::TextContent{.text = "OK."}},
        },
    };
    json j = r;
    EXPECT_EQ(j["description"], "A summary prompt");
    EXPECT_EQ(j["messages"].size(), 2u);
    auto back = j.get<mcp::GetPromptResult>();
    EXPECT_EQ(back.messages.size(), 2u);
    EXPECT_EQ(back.messages[0].role, mcp::Role::user);
}

TEST(Prompts, GetPromptParamsArgumentsAreStringMap) {
    mcp::GetPromptRequestParams p{
        .name      = "x",
        .arguments = std::unordered_map<std::string, std::string>{
            {"a", "1"}, {"b", "two"},
        },
    };
    json j = p;
    EXPECT_EQ(j["name"], "x");
    EXPECT_EQ(j["arguments"]["a"], "1");
    EXPECT_EQ(j["arguments"]["b"], "two");
    auto back = j.get<mcp::GetPromptRequestParams>();
    ASSERT_TRUE(back.arguments.has_value());
    EXPECT_EQ((*back.arguments)["a"], "1");
}

// -------------------------------------------------------------------------
// Server + Client integration
// -------------------------------------------------------------------------

struct PromptsServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread                  thread;

    template <typename Configure>
    PromptsServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "prompts-test", .version = "1.0",
          })) {
        cfg(*server);
        thread = std::thread([s = server, t = std::move(t)]() mutable {
            s->run(std::move(t));
        });
    }
    ~PromptsServerThread() {
        server->stop();
        if (thread.joinable()) thread.join();
    }
};

TEST(PromptsIntegration, ListPromptsAndGetPrompt) {
    auto p = mcp::test::make_in_memory_pair();
    PromptsServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.prompt(mcp::Prompt{
                .name        = "greet",
                .title       = "Greet",
                .description = "Greet someone by name",
                .arguments   = std::vector<mcp::PromptArgument>{
                    {.name = "name", .required = true},
                },
            },
            [](const std::unordered_map<std::string, std::string>& args) {
                const std::string name = args.contains("name")
                    ? args.at("name") : "world";
                return mcp::GetPromptResult{
                    .description = "greeting",
                    .messages = {
                        mcp::PromptMessage{
                            .role    = mcp::Role::user,
                            .content = mcp::TextContent{
                                .text = "Hello, " + name + "!"
                            },
                        },
                    },
                };
            });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    auto info = client.initialize().get();
    ASSERT_TRUE(info.capabilities.prompts.has_value());

    auto list = client.list_prompts().get();
    ASSERT_EQ(list.prompts.size(), 1u);
    EXPECT_EQ(list.prompts[0].name, "greet");
    ASSERT_TRUE(list.prompts[0].arguments.has_value());

    auto out = client.get_prompt(
        "greet",
        std::unordered_map<std::string, std::string>{{"name", "Alice"}}
    ).get();
    ASSERT_EQ(out.messages.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<mcp::TextContent>(out.messages[0].content));
    EXPECT_NE(std::get<mcp::TextContent>(out.messages[0].content).text.find("Alice"),
              std::string::npos);

    client.disconnect();
}

TEST(PromptsIntegration, MissingPromptMapsToError) {
    auto p = mcp::test::make_in_memory_pair();
    PromptsServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.prompt(mcp::Prompt{.name = "x"},
            [](const std::unordered_map<std::string, std::string>&) {
                return mcp::GetPromptResult{};
            });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto fut = client.get_prompt("nope");
    try {
        (void)fut.get();
        FAIL() << "expected Error";
    } catch (const mcp::Error& e) {
        EXPECT_EQ(e.code(), mcp::error_code::method_not_found);
    }
    client.disconnect();
}

TEST(PromptsIntegration, EmptyArgumentsAreOkay) {
    auto p = mcp::test::make_in_memory_pair();
    PromptsServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.prompt(mcp::Prompt{.name = "noargs"},
            [](const std::unordered_map<std::string, std::string>& args) {
                EXPECT_TRUE(args.empty());
                return mcp::GetPromptResult{
                    .messages = {
                        mcp::PromptMessage{
                            .role    = mcp::Role::user,
                            .content = mcp::TextContent{.text = "ok"},
                        },
                    },
                };
            });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto out = client.get_prompt("noargs").get();
    EXPECT_EQ(out.messages.size(), 1u);
    client.disconnect();
}

}  // namespace
