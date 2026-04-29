// SPDX-License-Identifier: Apache-2.0
//
// Phase 4a tests: elicitation. Cover both wire-shape round-trips and
// the server-initiated-with-handler integration path.

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

struct ServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread                  thread;

    template <typename Configure>
    ServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "elicit-srv", .version = "1.0",
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
// Wire-shape round trips
// -------------------------------------------------------------------------

TEST(Elicitation, FormParamsRoundTrip) {
    mcp::ElicitFormRequestParams p{
        .message          = "what's your name?",
        .requested_schema = json{{"type", "object"},
                                 {"properties", {{"name", {{"type", "string"}}}}},
                                 {"required",   json::array({"name"})}},
    };
    json j = p;
    EXPECT_EQ(j["message"], "what's your name?");
    EXPECT_FALSE(j.contains("mode"));   // form is the omitted-default
    auto back = j.get<mcp::ElicitFormRequestParams>();
    EXPECT_EQ(back.message, p.message);
    EXPECT_EQ(back.requested_schema["properties"]["name"]["type"], "string");
}

TEST(Elicitation, UrlParamsRoundTrip) {
    mcp::ElicitUrlRequestParams p{
        .message        = "please authorise",
        .url            = "https://example.com/auth?session=xyz",
        .elicitation_id = "abc-123",
    };
    json j = p;
    EXPECT_EQ(j["mode"], "url");
    EXPECT_EQ(j["url"], "https://example.com/auth?session=xyz");
    auto back = j.get<mcp::ElicitUrlRequestParams>();
    EXPECT_EQ(back.elicitation_id, "abc-123");
}

TEST(Elicitation, VariantTagDispatch) {
    // Form mode: omitted "mode"
    json form_wire = json{
        {"message",         "msg"},
        {"requestedSchema", json{{"type", "object"}}},
    };
    auto v = form_wire.get<mcp::ElicitRequestParams>();
    ASSERT_TRUE(std::holds_alternative<mcp::ElicitFormRequestParams>(v));

    // URL mode
    json url_wire = json{
        {"mode",          "url"},
        {"message",       "go"},
        {"url",           "https://example.com"},
        {"elicitationId", "id-1"},
    };
    auto v2 = url_wire.get<mcp::ElicitRequestParams>();
    ASSERT_TRUE(std::holds_alternative<mcp::ElicitUrlRequestParams>(v2));

    // Unknown mode is rejected (so a future "mode": "voice" doesn't
    // get silently parsed as a form).
    json bad = json{{"mode", "voice"}, {"message", "?"}};
    EXPECT_THROW((void)bad.get<mcp::ElicitRequestParams>(), mcp::Error);
}

TEST(Elicitation, ResultRoundTrip) {
    mcp::ElicitResult r{
        .action  = mcp::ElicitAction::accept,
        .content = json{{"name", "Alice"}, {"age", 30}},
    };
    json j = r;
    EXPECT_EQ(j["action"], "accept");
    EXPECT_EQ(j["content"]["name"], "Alice");
    auto back = j.get<mcp::ElicitResult>();
    EXPECT_EQ(back.action, mcp::ElicitAction::accept);
    ASSERT_TRUE(back.content.has_value());
    EXPECT_EQ((*back.content)["age"], 30);

    // Decline / cancel: content omitted on the wire.
    mcp::ElicitResult r2{.action = mcp::ElicitAction::decline};
    json j2 = r2;
    EXPECT_FALSE(j2.contains("content"));
    EXPECT_EQ(j2["action"], "decline");
    auto back2 = j2.get<mcp::ElicitResult>();
    EXPECT_EQ(back2.action, mcp::ElicitAction::decline);
    EXPECT_FALSE(back2.content.has_value());

    // Unknown action string ⇒ Error.
    json bad = json{{"action", "nope"}};
    EXPECT_THROW((void)bad.get<mcp::ElicitResult>(), mcp::Error);
}

TEST(Elicitation, CompleteNotificationParamsRoundTrip) {
    mcp::ElicitationCompleteNotificationParams p{
        .elicitation_id = "abc",
    };
    json j = p;
    EXPECT_EQ(j["elicitationId"], "abc");
    EXPECT_EQ(j.get<mcp::ElicitationCompleteNotificationParams>().elicitation_id,
              "abc");
}

// -------------------------------------------------------------------------
// Integration: tool handler issues an elicitation, client responds
// -------------------------------------------------------------------------

TEST(ElicitationIntegration, FormModeRoundTripThroughClient) {
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("ask_name", json{{"type", "object"}},
               [&s](const json&) -> mcp::CallToolResult {
                   auto fut = s.elicit(mcp::ElicitFormRequestParams{
                       .message = "name?",
                       .requested_schema = json{
                           {"type", "object"},
                           {"properties",
                            {{"name", {{"type", "string"}}}}},
                       },
                   });
                   auto er = fut.get();
                   if (er.action != mcp::ElicitAction::accept ||
                       !er.content.has_value()) {
                       return mcp::CallToolResult{
                           .content = { mcp::TextContent{.text = "no answer"} },
                           .is_error = true,
                       };
                   }
                   const std::string nm =
                       er.content->value("name", std::string{"<none>"});
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{.text = "hi " + nm} },
                   };
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_elicitation_handler(
        [](const mcp::ElicitRequestParams& req) -> mcp::ElicitResult {
            // Should be form mode in this test.
            EXPECT_TRUE(std::holds_alternative<mcp::ElicitFormRequestParams>(req));
            const auto& f = std::get<mcp::ElicitFormRequestParams>(req);
            EXPECT_EQ(f.message, "name?");
            return mcp::ElicitResult{
                .action  = mcp::ElicitAction::accept,
                .content = json{{"name", "Ada"}},
            };
        });
    auto info = client.initialize().get();
    ASSERT_TRUE(info.capabilities.tools.has_value());
    // The capability we declared should propagate through initialize.
    // (The server reflects ours back to us via... no, it doesn't —
    //  but our local advertise was set under handlers_mu_, so the
    //  initialize payload included `elicitation: {form:{}, url:{}}`.)

    auto out = client.call_tool("ask_name").get();
    ASSERT_FALSE(out.is_error.value_or(false));
    ASSERT_EQ(out.content.size(), 1u);
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "hi Ada");

    client.disconnect();
}

TEST(ElicitationIntegration, DeclineCarriesThrough) {
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("touch", json{{"type", "object"}},
               [&s](const json&) -> mcp::CallToolResult {
                   auto er = s.elicit(mcp::ElicitFormRequestParams{
                       .message = "?",
                       .requested_schema = json{{"type", "object"}},
                   }).get();
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{
                           .text = (er.action == mcp::ElicitAction::decline)
                                       ? "declined" : "other",
                       }},
                   };
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_elicitation_handler(
        [](const mcp::ElicitRequestParams&) -> mcp::ElicitResult {
            return mcp::ElicitResult{.action = mcp::ElicitAction::decline};
        });
    (void)client.initialize().get();

    auto out = client.call_tool("touch").get();
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "declined");

    client.disconnect();
}

TEST(ElicitationIntegration, MissingHandlerYieldsError) {
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("touch", json{{"type", "object"}},
               [&s](const json&) -> mcp::CallToolResult {
                   try {
                       (void)s.elicit(mcp::ElicitFormRequestParams{
                           .message = "?",
                           .requested_schema = json{{"type", "object"}},
                       }).get();
                   } catch (const mcp::Error&) {
                       return mcp::CallToolResult{
                           .content = { mcp::TextContent{.text = "no-elicit"} },
                           .is_error = true,
                       };
                   }
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{.text = "unexpected-success"} },
                   };
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));   // no elicitation handler
    (void)client.initialize().get();

    auto out = client.call_tool("touch").get();
    ASSERT_TRUE(out.is_error.value_or(false));
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "no-elicit");

    client.disconnect();
}

TEST(ElicitationIntegration, UrlModeNotificationDeliversToClient) {
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("kick", json{{"type", "object"}},
               [&s](const json&) -> mcp::CallToolResult {
                   const auto ec = s.notify_elicitation_complete("eid-77");
                   if (ec) {
                       return mcp::CallToolResult{
                           .content = { mcp::TextContent{
                               .text = std::string{"notify failed: "} + ec.message(),
                           }},
                           .is_error = true,
                       };
                   }
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{.text = "kicked"} },
                   };
               });
    });

    std::promise<std::string> got;
    auto fut = got.get_future();
    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_elicitation_complete_handler(
        [&](std::string id) {
            try { got.set_value(std::move(id)); } catch (...) {}
        });
    (void)client.initialize().get();

    auto out = client.call_tool("kick").get();
    ASSERT_FALSE(out.is_error.value_or(false));
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "kicked");

    ASSERT_EQ(fut.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(fut.get(), "eid-77");

    client.disconnect();
}

TEST(ElicitationIntegration, UrlModeFullRoundTripWithCompletion) {
    // Audit fix: the previous URL-mode test only fired a stand-alone
    // notification; it never issued an actual URL-mode elicitation
    // through Server::elicit. This test does the real thing:
    //   server.elicit(URL params) → client returns accept →
    //   server emits notifications/elicitation/complete →
    //   client receives the completion.
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("oauth_kickoff", json{{"type", "object"}},
               [&s](const json&) -> mcp::CallToolResult {
                   auto er = s.elicit(mcp::ElicitUrlRequestParams{
                       .message        = "Authorise via the linked URL",
                       .url            = "https://idp.example/auth?code=xyz",
                       .elicitation_id = "eid-url-1",
                   }).get();
                   if (er.action != mcp::ElicitAction::accept) {
                       return mcp::CallToolResult{
                           .content = { mcp::TextContent{.text = "no"} },
                           .is_error = true,
                       };
                   }
                   (void)s.notify_elicitation_complete("eid-url-1");
                   return mcp::CallToolResult{
                       .content = { mcp::TextContent{.text = "ok"} },
                   };
               });
    });

    std::promise<std::string> completion_p;
    auto completion_fut = completion_p.get_future();

    bool got_url_mode_request = false;
    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_elicitation_handler(
        [&](const mcp::ElicitRequestParams& req) -> mcp::ElicitResult {
            EXPECT_TRUE(std::holds_alternative<mcp::ElicitUrlRequestParams>(req));
            if (std::holds_alternative<mcp::ElicitUrlRequestParams>(req)) {
                const auto& u = std::get<mcp::ElicitUrlRequestParams>(req);
                EXPECT_EQ(u.elicitation_id, "eid-url-1");
                EXPECT_NE(u.url.find("idp.example"), std::string::npos);
                got_url_mode_request = true;
            }
            return mcp::ElicitResult{.action = mcp::ElicitAction::accept};
        });
    client.set_elicitation_complete_handler(
        [&](std::string id) {
            try { completion_p.set_value(std::move(id)); } catch (...) {}
        });
    (void)client.initialize().get();

    auto out = client.call_tool("oauth_kickoff").get();
    ASSERT_FALSE(out.is_error.value_or(false));
    EXPECT_TRUE(got_url_mode_request);

    ASSERT_EQ(completion_fut.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(completion_fut.get(), "eid-url-1");

    client.disconnect();
}

TEST(ElicitationIntegration, ServerReceivesElicitationComplete) {
    // Audit fix: the spec lets EITHER side emit
    // notifications/elicitation/complete. The server side previously
    // had no API to receive it. Verify the full round-trip:
    //   client.notify_elicitation_complete(id) →
    //   server's set_elicitation_complete_handler fires with id.
    auto p = mcp::test::make_in_memory_pair();

    std::promise<std::string> server_got;
    auto server_fut = server_got.get_future();

    ServerThread srv(std::move(p.b), [&](mcp::Server& s) {
        s.set_elicitation_complete_handler(
            [&](std::string id) {
                try { server_got.set_value(std::move(id)); } catch (...) {}
            });
    });

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    const auto ec = client.notify_elicitation_complete("eid-from-client");
    EXPECT_FALSE(ec) << "send_notification failed: " << ec.message();

    ASSERT_EQ(server_fut.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(server_fut.get(), "eid-from-client");

    client.disconnect();
}

TEST(ElicitationIntegration, CapabilityActuallyAdvertisedOnInitialize) {
    // Audit fix: the FormModeRoundTripThroughClient test admitted
    // it never verified the capability was advertised. This test
    // captures the inbound InitializeRequestParams server-side
    // and asserts the elicitation capability was sent.
    auto p = mcp::test::make_in_memory_pair();

    std::promise<mcp::ClientCapabilities> caps_p;
    auto caps_fut = caps_p.get_future();

    // We need access to the request params before the server's
    // built-in initialize handler consumes them. Use a raw Session
    // on the b-side instead of a Server, so we can install our own
    // initialize handler that captures + responds.
    auto b_session = std::make_unique<mcp::Session>(std::move(p.b));
    b_session->set_request_handler("initialize",
        [&](const nlohmann::json& params) -> nlohmann::json {
            auto parsed = params.get<mcp::InitializeRequestParams>();
            try { caps_p.set_value(parsed.capabilities); } catch (...) {}
            return mcp::InitializeResult{
                .protocol_version = std::string{mcp::kLatestProtocolVersion},
                .capabilities     = mcp::ServerCapabilities{},
                .server_info      = mcp::Implementation{
                    .name = "x", .version = "0",
                },
            };
        });
    b_session->start();

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_elicitation_handler(
        [](const mcp::ElicitRequestParams&) -> mcp::ElicitResult {
            return mcp::ElicitResult{.action = mcp::ElicitAction::accept};
        });

    (void)client.initialize().get();

    ASSERT_EQ(caps_fut.wait_for(2s), std::future_status::ready);
    auto caps = caps_fut.get();
    ASSERT_TRUE(caps.elicitation.has_value());
    EXPECT_TRUE(caps.elicitation->form.has_value());
    EXPECT_TRUE(caps.elicitation->url.has_value());

    client.disconnect();
    b_session->close();
}

TEST(ElicitationIntegration, NarrowedCapabilityOverrideKeepsOtherDerivedCaps) {
    // Audit fix: set_client_capabilities() was a *full replacement*,
    // dropping handler-derived sampling/roots/elicitation. Now it's
    // a per-field override — engaged fields replace, unset fields
    // keep their derived values.
    auto p = mcp::test::make_in_memory_pair();

    std::promise<mcp::ClientCapabilities> caps_p;
    auto caps_fut = caps_p.get_future();

    auto b_session = std::make_unique<mcp::Session>(std::move(p.b));
    b_session->set_request_handler("initialize",
        [&](const nlohmann::json& params) -> nlohmann::json {
            auto parsed = params.get<mcp::InitializeRequestParams>();
            try { caps_p.set_value(parsed.capabilities); } catch (...) {}
            return mcp::InitializeResult{
                .protocol_version = std::string{mcp::kLatestProtocolVersion},
                .capabilities     = mcp::ServerCapabilities{},
                .server_info      = mcp::Implementation{
                    .name = "x", .version = "0",
                },
            };
        });
    b_session->start();

    mcp::Client client{ mcp::Implementation{.name = "tester", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_sampling_handler(
        [](const mcp::CreateMessageRequestParams&) -> mcp::CreateMessageResult {
            return {.role = mcp::Role::assistant, .model = "m"};
        });
    client.set_elicitation_handler(
        [](const mcp::ElicitRequestParams&) -> mcp::ElicitResult {
            return {.action = mcp::ElicitAction::accept};
        });
    // Override: narrow elicitation to form-only. Sampling override
    // not set — should keep the handler-derived value.
    mcp::ClientCapabilities override_caps;
    mcp::ElicitationCapability ec;
    ec.form = nlohmann::json::object();
    // ec.url left absent ⇒ form-only.
    override_caps.elicitation = std::move(ec);
    client.set_client_capabilities(std::move(override_caps));

    (void)client.initialize().get();
    ASSERT_EQ(caps_fut.wait_for(2s), std::future_status::ready);
    auto caps = caps_fut.get();
    ASSERT_TRUE(caps.elicitation.has_value());
    EXPECT_TRUE(caps.elicitation->form.has_value());
    EXPECT_FALSE(caps.elicitation->url.has_value());
    // The sampling capability survived because we didn't override it.
    EXPECT_TRUE(caps.sampling.has_value());

    client.disconnect();
    b_session->close();
}

TEST(Elicitation, ModeNullRejected) {
    json j = json{{"mode", nullptr}, {"message", "?"}};
    EXPECT_THROW((void)j.get<mcp::ElicitRequestParams>(), mcp::Error);
}

TEST(Elicitation, RequestedSchemaMustBeObject) {
    json j = json{
        {"message",         "?"},
        {"requestedSchema", "not-an-object"},
    };
    EXPECT_THROW((void)j.get<mcp::ElicitFormRequestParams>(), mcp::Error);

    json j2 = json{
        {"message",         "?"},
        {"requestedSchema", nullptr},
    };
    EXPECT_THROW((void)j2.get<mcp::ElicitFormRequestParams>(), mcp::Error);
}

}  // namespace
