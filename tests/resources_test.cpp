// SPDX-License-Identifier: Apache-2.0
//
// Round-trip and integration tests for the resources surface added in
// Phase 2: list_resources, list_resource_templates, read_resource,
// subscribe/unsubscribe, plus the resource-related content blocks
// (ResourceLink, EmbeddedResource).

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
#include <variant>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

// -------------------------------------------------------------------------
// Round-trip tests for the new types
// -------------------------------------------------------------------------

TEST(Resources, TextContentsRoundTrip) {
    mcp::TextResourceContents c{
        .uri       = "file:///hi.txt",
        .mime_type = "text/plain",
        .text      = "hi",
    };
    json j = c;
    EXPECT_EQ(j["uri"],      "file:///hi.txt");
    EXPECT_EQ(j["mimeType"], "text/plain");
    EXPECT_EQ(j["text"],     "hi");
    auto back = j.get<mcp::TextResourceContents>();
    EXPECT_EQ(back.uri,  c.uri);
    EXPECT_EQ(back.text, c.text);
}

TEST(Resources, BlobContentsRoundTrip) {
    mcp::BlobResourceContents c{
        .uri       = "file:///bin",
        .mime_type = "application/octet-stream",
        .blob      = "Zm9v",
    };
    json j = c;
    EXPECT_EQ(j["blob"], "Zm9v");
    auto back = j.get<mcp::BlobResourceContents>();
    EXPECT_EQ(back.blob, c.blob);
}

TEST(Resources, ContentsVariantDispatchesByDiscriminator) {
    json text_obj = {{"uri", "x"}, {"text", "y"}};
    auto v = text_obj.get<mcp::ResourceContents>();
    EXPECT_TRUE(std::holds_alternative<mcp::TextResourceContents>(v));

    json blob_obj = {{"uri", "x"}, {"blob", "Zm9v"}};
    v = blob_obj.get<mcp::ResourceContents>();
    EXPECT_TRUE(std::holds_alternative<mcp::BlobResourceContents>(v));

    json bad = {{"uri", "x"}};
    EXPECT_THROW(bad.get<mcp::ResourceContents>(), mcp::Error);
}

TEST(Resources, ResourceRoundTrip) {
    mcp::Resource r{
        .uri       = "file:///hi",
        .name      = "hi",
        .title     = "Hi",
        .mime_type = "text/plain",
        .size      = 10,
    };
    json j = r;
    EXPECT_EQ(j["uri"], "file:///hi");
    EXPECT_EQ(j["mimeType"], "text/plain");
    EXPECT_EQ(j["size"], 10);
    auto back = j.get<mcp::Resource>();
    EXPECT_EQ(back.uri, r.uri);
    EXPECT_EQ(back.size, r.size);
}

TEST(Resources, ResourceTemplateRoundTrip) {
    mcp::ResourceTemplate r{
        .uri_template = "file:///{name}.txt",
        .name         = "files",
    };
    json j = r;
    EXPECT_EQ(j["uriTemplate"], "file:///{name}.txt");
    auto back = j.get<mcp::ResourceTemplate>();
    EXPECT_EQ(back.uri_template, r.uri_template);
}

TEST(Resources, ResourceLinkContentBlock) {
    mcp::ResourceLink rl{
        .uri  = "file:///doc",
        .name = "Doc",
    };
    json j = nlohmann::json(mcp::ContentBlock{rl});
    EXPECT_EQ(j["type"], "resource_link");
    EXPECT_EQ(j["uri"],  "file:///doc");

    auto block = j.get<mcp::ContentBlock>();
    ASSERT_TRUE(std::holds_alternative<mcp::ResourceLink>(block));
    EXPECT_EQ(std::get<mcp::ResourceLink>(block).uri, rl.uri);
}

TEST(Resources, EmbeddedResourceContentBlock) {
    mcp::EmbeddedResource er{
        .resource = mcp::TextResourceContents{
            .uri       = "file:///r",
            .mime_type = "text/plain",
            .text      = "embedded",
        },
    };
    json j = nlohmann::json(mcp::ContentBlock{er});
    EXPECT_EQ(j["type"], "resource");
    EXPECT_EQ(j["resource"]["text"], "embedded");

    auto block = j.get<mcp::ContentBlock>();
    ASSERT_TRUE(std::holds_alternative<mcp::EmbeddedResource>(block));
    auto inner = std::get<mcp::EmbeddedResource>(block);
    EXPECT_TRUE(std::holds_alternative<mcp::TextResourceContents>(inner.resource));
}

// -------------------------------------------------------------------------
// Server + Client integration
// -------------------------------------------------------------------------

struct ResourcesServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread                  thread;

    template <typename Configure>
    ResourcesServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "resources-test", .version = "1.0",
          })) {
        cfg(*server);
        thread = std::thread([s = server, t = std::move(t)]() mutable {
            s->run(std::move(t));
        });
    }
    ~ResourcesServerThread() {
        server->stop();
        if (thread.joinable()) thread.join();
    }
};

TEST(ResourcesIntegration, ListAndRead) {
    auto p = mcp::test::make_in_memory_pair();
    ResourcesServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.resource(mcp::Resource{
                .uri = "file:///hello.txt",
                .name = "hello",
                .mime_type = "text/plain",
            },
            [](const std::string& uri) {
                return mcp::ReadResourceResult{
                    .contents = {
                        mcp::TextResourceContents{
                            .uri       = uri,
                            .mime_type = "text/plain",
                            .text      = "hello, world\n",
                        }
                    },
                };
            });
        s.resource(mcp::Resource{
                .uri = "file:///bin",
                .name = "bin",
                .mime_type = "application/octet-stream",
            },
            [](const std::string& uri) {
                return mcp::ReadResourceResult{
                    .contents = {
                        mcp::BlobResourceContents{
                            .uri       = uri,
                            .mime_type = "application/octet-stream",
                            .blob      = "Zm9vYmFy",
                        }
                    },
                };
            });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    auto info = client.initialize().get();
    ASSERT_TRUE(info.capabilities.resources.has_value());

    auto list = client.list_resources().get();
    EXPECT_EQ(list.resources.size(), 2u);

    auto read = client.read_resource("file:///hello.txt").get();
    ASSERT_EQ(read.contents.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<mcp::TextResourceContents>(read.contents[0]));
    EXPECT_EQ(std::get<mcp::TextResourceContents>(read.contents[0]).text,
              "hello, world\n");

    auto blob = client.read_resource("file:///bin").get();
    ASSERT_EQ(blob.contents.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<mcp::BlobResourceContents>(blob.contents[0]));

    client.disconnect();
}

TEST(ResourcesIntegration, MissingResourceMapsToError) {
    auto p = mcp::test::make_in_memory_pair();
    ResourcesServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.resource(mcp::Resource{.uri = "file:///x", .name = "x"},
            [](const std::string&) {
                return mcp::ReadResourceResult{};
            });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto fut = client.read_resource("file:///nope");
    try {
        (void)fut.get();
        FAIL() << "expected Error";
    } catch (const mcp::Error& e) {
        EXPECT_EQ(e.code(), mcp::error_code::method_not_found);
    }

    client.disconnect();
}

TEST(ResourcesIntegration, FallbackHandlesUnknownUri) {
    auto p = mcp::test::make_in_memory_pair();
    ResourcesServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.fallback_resource_handler([](const std::string& uri) {
            return mcp::ReadResourceResult{
                .contents = {
                    mcp::TextResourceContents{
                        .uri  = uri,
                        .text = "fallback for " + uri,
                    }
                },
            };
        });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto read = client.read_resource("file:///anything").get();
    ASSERT_EQ(read.contents.size(), 1u);
    EXPECT_NE(std::get<mcp::TextResourceContents>(read.contents[0]).text.find("fallback"),
              std::string::npos);

    client.disconnect();
}

TEST(ResourcesIntegration, ResourceTemplateIsReturned) {
    auto p = mcp::test::make_in_memory_pair();
    ResourcesServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.resource_template(mcp::ResourceTemplate{
            .uri_template = "file:///{name}.txt",
            .name         = "files",
        });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto list = client.list_resource_templates().get();
    ASSERT_EQ(list.resource_templates.size(), 1u);
    EXPECT_EQ(list.resource_templates[0].uri_template, "file:///{name}.txt");

    client.disconnect();
}

}  // namespace
