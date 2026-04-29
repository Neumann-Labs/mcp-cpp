// SPDX-License-Identifier: Apache-2.0
//
// Pagination wiring tests. The server caps responses at `page_size`,
// returning a `nextCursor` so the client can fetch the remainder.

#include "in_memory_transport.hpp"

#include "mcp/client.hpp"
#include "mcp/protocol.hpp"
#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <set>
#include <string>
#include <thread>

namespace {

using nlohmann::json;

struct ServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread                  thread;

    template <typename Configure>
    ServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "pagi", .version = "1.0",
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

TEST(Pagination, ToolsListReturnsPagesWithCursor) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.set_page_size(3);
        for (int i = 0; i < 7; ++i) {
            s.tool("t" + std::to_string(i),
                   json{{"type", "object"}},
                   [](const json&) -> mcp::CallToolResult {
                       return {.content = { mcp::TextContent{.text = "ok"} }};
                   });
        }
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    std::set<std::string> seen;
    std::optional<std::string> cursor;
    int pages = 0;
    do {
        auto res = client.list_tools(cursor).get();
        EXPECT_LE(res.tools.size(), 3u);
        for (const auto& t : res.tools) seen.insert(t.name);
        cursor = res.next_cursor;
        ++pages;
        ASSERT_LT(pages, 10) << "infinite loop in pagination";
    } while (cursor.has_value());

    EXPECT_GE(pages, 3);
    EXPECT_EQ(seen.size(), 7u);
    client.disconnect();
}

TEST(Pagination, NoPaginationByDefault) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        for (int i = 0; i < 5; ++i) {
            s.tool("t" + std::to_string(i),
                   json{{"type", "object"}},
                   [](const json&) -> mcp::CallToolResult {
                       return {.content = { mcp::TextContent{.text = "ok"} }};
                   });
        }
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto res = client.list_tools().get();
    EXPECT_EQ(res.tools.size(), 5u);
    EXPECT_FALSE(res.next_cursor.has_value());
    client.disconnect();
}

TEST(Pagination, ResourcesPaginate) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.set_page_size(2);
        for (int i = 0; i < 5; ++i) {
            s.resource(mcp::Resource{
                    .uri  = "file:///x" + std::to_string(i),
                    .name = "n" + std::to_string(i),
                },
                [](const std::string& uri) {
                    return mcp::ReadResourceResult{
                        .contents = { mcp::TextResourceContents{.uri = uri, .text = "y"} },
                    };
                });
        }
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    int total = 0;
    std::optional<std::string> cursor;
    int pages = 0;
    do {
        auto res = client.list_resources(cursor).get();
        total += static_cast<int>(res.resources.size());
        cursor = res.next_cursor;
        ++pages;
        ASSERT_LT(pages, 10);
    } while (cursor.has_value());

    EXPECT_EQ(total, 5);
    client.disconnect();
}

TEST(Pagination, MalformedCursorRejected) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.set_page_size(2);
        for (int i = 0; i < 5; ++i) {
            s.tool("t" + std::to_string(i),
                   json{{"type", "object"}},
                   [](const json&) -> mcp::CallToolResult {
                       return {.content = { mcp::TextContent{.text = "ok"} }};
                   });
        }
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto fut = client.list_tools(std::string{"not-a-number"});
    try {
        (void)fut.get();
        FAIL() << "expected Error for bad cursor";
    } catch (const mcp::Error& e) {
        EXPECT_EQ(e.code(), mcp::error_code::invalid_params);
    }
    client.disconnect();
}

}  // namespace
