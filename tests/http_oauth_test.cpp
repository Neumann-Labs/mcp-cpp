// SPDX-License-Identifier: Apache-2.0
//
// Phase 4c tests: OAuth 2.1 authorization on the HTTP transport.
//
// We use a raw httplib::Client for the negative paths (sending a
// raw POST without — or with the wrong — Authorization header), and
// the regular HttpClientTransport + Client stack for the positive
// path (Bearer-attached requests pass through end-to-end).

#if !defined(MCP_ENABLE_HTTP)
#error "MCP_ENABLE_HTTP must be defined for http_oauth_test"
#endif

#include "mcp/client.hpp"
#include "mcp/http_client_transport.hpp"
#include "mcp/http_server_host.hpp"
#include "mcp/protocol.hpp"

#include <gtest/gtest.h>

#include <httplib.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

TEST(HttpOAuth, MissingAuthorizationProduces401) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator = [](std::string_view) {
                return mcp::HttpServerHost::Options::BearerOutcome{
                    .status = mcp::HttpServerHost::Options::BearerStatus::invalid_token,
                };
            },
            .auth_realm       = "test-realm",
            .resource_metadata_url =
                "https://example.com/.well-known/oauth-protected-resource",
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    auto res = cli.Post("/mcp",
        {{"Content-Type", "application/json"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})",
        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
    auto it = res->headers.find("WWW-Authenticate");
    ASSERT_NE(it, res->headers.end());
    EXPECT_NE(it->second.find("Bearer"),                  std::string::npos);
    EXPECT_NE(it->second.find("realm=\"test-realm\""),    std::string::npos);
    EXPECT_NE(it->second.find("resource_metadata=\"https://example.com"),
              std::string::npos);

    host.stop();
}

TEST(HttpOAuth, InvalidBearerProduces401) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](std::string_view t) {
                    using O = mcp::HttpServerHost::Options;
                    return O::BearerOutcome{
                        .status = (t == "good") ? O::BearerStatus::allow
                                                : O::BearerStatus::invalid_token,
                    };
                },
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    auto res = cli.Post("/mcp",
        {{"Content-Type",  "application/json"},
         {"Authorization", "Bearer wrong-token"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})",
        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);

    host.stop();
}

TEST(HttpOAuth, NonBearerSchemeRejected) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator = [](std::string_view) {
                return mcp::HttpServerHost::Options::BearerOutcome{
                    .status = mcp::HttpServerHost::Options::BearerStatus::allow,
                };
            },
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    auto res = cli.Post("/mcp",
        {{"Content-Type",  "application/json"},
         {"Authorization", "Basic dXNlcjpwYXNz"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})",
        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);

    host.stop();
}

TEST(HttpOAuth, ValidBearerEndToEnd) {
    std::atomic<int> validations{0};
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "calc", .version = "1.0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [&](std::string_view t) {
                    validations.fetch_add(1, std::memory_order_relaxed);
                    using O = mcp::HttpServerHost::Options;
                    return O::BearerOutcome{
                        .status = (t == "secret") ? O::BearerStatus::allow
                                                  : O::BearerStatus::invalid_token,
                    };
                },
        },
        [](mcp::Server& s) {
            s.tool("ping", json{{"type", "object"}},
                   [](const json&) -> mcp::CallToolResult {
                       return {.content = { mcp::TextContent{.text = "pong"} }};
                   });
        },
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url             = "http://127.0.0.1:" + std::to_string(host.port())
                           + "/mcp";
    topts.access_token    = "secret";
    topts.open_get_stream = false;

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::make_unique<mcp::HttpClientTransport>(topts));

    (void)client.initialize().get();
    auto out = client.call_tool("ping").get();
    ASSERT_FALSE(out.is_error.value_or(false));
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "pong");

    // At least one validation per inbound POST. We sent at least
    // initialize + tools/call.
    EXPECT_GE(validations.load(), 2);

    client.disconnect();
    host.stop();
}

TEST(HttpOAuth, ClientReceives401Callback) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator = [](std::string_view) {
                return mcp::HttpServerHost::Options::BearerOutcome{
                    .status = mcp::HttpServerHost::Options::BearerStatus::invalid_token,
                };
            },
            .resource_metadata_url =
                "https://idp.example/.well-known/oauth-protected-resource",
        },
        [](mcp::Server&) {},
    };
    host.start();

    std::promise<std::string> challenge_p;
    auto challenge_f = challenge_p.get_future();

    mcp::HttpClientTransport::Options topts;
    topts.url             = "http://127.0.0.1:" + std::to_string(host.port())
                           + "/mcp";
    topts.access_token    = "anything";
    topts.open_get_stream = false;
    topts.on_unauthorized = [&](int, std::string_view ch) {
        try { challenge_p.set_value(std::string{ch}); } catch (...) {}
    };

    auto transport = std::make_unique<mcp::HttpClientTransport>(topts);
    transport->start();
    EXPECT_FALSE(transport->send(
        R"({"jsonrpc":"2.0","method":"initialize","id":1})"));

    ASSERT_EQ(challenge_f.wait_for(2s), std::future_status::ready);
    const auto challenge = challenge_f.get();
    EXPECT_NE(challenge.find("Bearer"),         std::string::npos);
    EXPECT_NE(challenge.find("resource_metadata=\"https://idp.example"),
              std::string::npos);

    transport->close();
    host.stop();
}

TEST(HttpOAuth, ResourceMetadataDocumentIsServed) {
    json metadata = {
        {"resource",                "https://example.com/mcp"},
        {"authorization_servers",   json::array({"https://idp.example"})},
        {"scopes_supported",        json::array({"mcp:read", "mcp:write"})},
        {"bearer_methods_supported", json::array({"header"})},
    };
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .resource_metadata = metadata,
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    // Root well-known URI.
    {
        auto res = cli.Get("/.well-known/oauth-protected-resource");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
        auto body = json::parse(res->body);
        EXPECT_EQ(body["resource"], "https://example.com/mcp");
        EXPECT_EQ(body["authorization_servers"][0], "https://idp.example");
    }
    // Path-prefixed variant.
    {
        auto res = cli.Get("/.well-known/oauth-protected-resource/mcp");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
    }

    host.stop();
}

TEST(HttpOAuth, MetadataDocumentNotServedWhenAbsent) {
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
    auto res = cli.Get("/.well-known/oauth-protected-resource");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    host.stop();
}

TEST(HttpOAuth, CaseInsensitiveBearerScheme) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator = [](std::string_view t) {
                using O = mcp::HttpServerHost::Options;
                return O::BearerOutcome{
                    .status = (t == "tok") ? O::BearerStatus::allow
                                            : O::BearerStatus::invalid_token,
                };
            },
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    // RFC 7235 §2.1: scheme is case-insensitive. Try lowercase + uppercase.
    for (const char* hdr : {"bearer tok", "BEARER tok", "BeArEr tok"}) {
        auto res = cli.Post("/mcp",
            {{"Content-Type", "application/json"},
             {"Authorization", hdr}},
            R"({"jsonrpc":"2.0","method":"initialize","id":1})",
            "application/json");
        ASSERT_TRUE(res);
        // We expect 400/401 only if the body fails session lookup, NOT 401
        // for scheme parsing.
        EXPECT_NE(res->status, 401) << "rejected scheme: " << hdr;
    }

    host.stop();
}

TEST(HttpOAuth, MultipleAuthorizationHeadersRejected) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator = [](std::string_view) {
                return mcp::HttpServerHost::Options::BearerOutcome{
                    .status = mcp::HttpServerHost::Options::BearerStatus::allow,
                };
            },
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    httplib::Headers hdrs = {
        {"Content-Type",  "application/json"},
        {"Authorization", "Bearer one"},
        {"Authorization", "Bearer two"},
    };
    auto res = cli.Post("/mcp", hdrs,
        R"({"jsonrpc":"2.0","method":"initialize","id":1})",
        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    host.stop();
}

TEST(HttpOAuth, InsufficientScopeProduces403) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator = [](std::string_view) {
                using O = mcp::HttpServerHost::Options;
                return O::BearerOutcome{
                    .status          = O::BearerStatus::insufficient_scope,
                    .required_scopes = "mcp:read mcp:tools",
                };
            },
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    auto res = cli.Post("/mcp",
        {{"Content-Type",  "application/json"},
         {"Authorization", "Bearer tok"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})",
        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
    auto it = res->headers.find("WWW-Authenticate");
    ASSERT_NE(it, res->headers.end());
    EXPECT_NE(it->second.find("error=\"insufficient_scope\""),  std::string::npos);
    EXPECT_NE(it->second.find("scope=\"mcp:read mcp:tools\""),  std::string::npos);

    host.stop();
}

TEST(HttpOAuth, ChallengeIncludesInvalidTokenError) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator = [](std::string_view) {
                return mcp::HttpServerHost::Options::BearerOutcome{
                    .status = mcp::HttpServerHost::Options::BearerStatus::invalid_token,
                };
            },
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    auto res = cli.Post("/mcp",
        {{"Content-Type",  "application/json"},
         {"Authorization", "Bearer junk"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})",
        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
    auto it = res->headers.find("WWW-Authenticate");
    ASSERT_NE(it, res->headers.end());
    EXPECT_NE(it->second.find("error=\"invalid_token\""), std::string::npos);

    host.stop();
}

TEST(HttpOAuth, EmptyBearerTokenRejected) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator = [](std::string_view) {
                return mcp::HttpServerHost::Options::BearerOutcome{
                    .status = mcp::HttpServerHost::Options::BearerStatus::allow,
                };
            },
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    // Trailing whitespace after "Bearer" is the only token68 — empty.
    for (const char* hdr : {"Bearer ", "Bearer  ", "Bearer \t"}) {
        auto res = cli.Post("/mcp",
            {{"Content-Type",  "application/json"},
             {"Authorization", hdr}},
            R"({"jsonrpc":"2.0","method":"initialize","id":1})",
            "application/json");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 401) << "header was: '" << hdr << "'";
    }

    host.stop();
}

TEST(HttpOAuth, MetadataDocumentHasCors) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .resource_metadata = json{{"resource", "https://x"}},
        },
        [](mcp::Server&) {},
    };
    host.start();

    httplib::Client cli{"http://127.0.0.1:" + std::to_string(host.port())};
    auto res = cli.Get("/.well-known/oauth-protected-resource");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto cors = res->headers.find("Access-Control-Allow-Origin");
    ASSERT_NE(cors, res->headers.end());
    EXPECT_EQ(cors->second, "*");

    host.stop();
}

TEST(HttpOAuth, RealmInjectionRejectedAtStart) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host       = "127.0.0.1",
            .path       = "/mcp",
            .bearer_validator = [](std::string_view) {
                return mcp::HttpServerHost::Options::BearerOutcome{
                    .status = mcp::HttpServerHost::Options::BearerStatus::allow,
                };
            },
            // CRLF + closing quote — header injection / smuggling.
            .auth_realm = "evil\r\nX-Pwn: yes",
        },
        [](mcp::Server&) {},
    };
    EXPECT_THROW(host.start(), std::invalid_argument);
}

TEST(HttpOAuth, MetadataUrlInjectionRejectedAtStart) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host                  = "127.0.0.1",
            .path                  = "/mcp",
            .bearer_validator      = [](std::string_view) {
                return mcp::HttpServerHost::Options::BearerOutcome{
                    .status = mcp::HttpServerHost::Options::BearerStatus::allow,
                };
            },
            .resource_metadata_url = "https://x.com/\".pwn",
        },
        [](mcp::Server&) {},
    };
    EXPECT_THROW(host.start(), std::invalid_argument);
}

TEST(HttpOAuth, ClientSetAccessTokenRotates) {
    std::atomic<int> valid_count{0};
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator = [&](std::string_view t) {
                using O = mcp::HttpServerHost::Options;
                if (t == "v2") {
                    valid_count.fetch_add(1, std::memory_order_relaxed);
                    return O::BearerOutcome{.status = O::BearerStatus::allow};
                }
                return O::BearerOutcome{.status = O::BearerStatus::invalid_token};
            },
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
    topts.access_token    = "v1";  // wrong
    topts.open_get_stream = false;
    auto* transport_raw   = new mcp::HttpClientTransport{topts};
    std::unique_ptr<mcp::HttpClientTransport> transport{transport_raw};

    EXPECT_EQ(transport_raw->access_token(), "v1");
    transport_raw->set_access_token("v2");
    EXPECT_EQ(transport_raw->access_token(), "v2");

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(transport));
    (void)client.initialize().get();
    auto out = client.call_tool("noop").get();
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "ok");
    EXPECT_GE(valid_count.load(), 2);

    client.disconnect();
    host.stop();
}

TEST(HttpOAuth, NoValidatorMeansNoAuth) {
    // Sanity check: existing code paths (no validator configured)
    // are unaffected — no Authorization header required.
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
    auto out = client.call_tool("noop").get();
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "ok");

    client.disconnect();
    host.stop();
}

}  // namespace
