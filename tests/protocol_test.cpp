// SPDX-License-Identifier: Apache-2.0
//
// Round-trip and shape tests for the protocol types. The strategy: for each
// type, build a non-trivial value, serialize to JSON, compare to a reference
// JSON shape from the spec, then deserialize back and verify equality.

#include "mcp/protocol.hpp"

#include "mcp/error.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <variant>

namespace {

using nlohmann::json;

// -------------------------------------------------------------------------
// RequestId
// -------------------------------------------------------------------------

TEST(RequestId, IntegerRoundTrip) {
    mcp::RequestId id{42};
    EXPECT_TRUE(id.is_integer());
    EXPECT_EQ(id.as_integer(), 42);
    json j = id;
    EXPECT_EQ(j, json(42));
    auto back = j.get<mcp::RequestId>();
    EXPECT_EQ(id, back);
}

TEST(RequestId, StringRoundTrip) {
    mcp::RequestId id{std::string{"abc"}};
    EXPECT_TRUE(id.is_string());
    EXPECT_EQ(id.as_string(), "abc");
    json j = id;
    EXPECT_EQ(j, json("abc"));
    auto back = j.get<mcp::RequestId>();
    EXPECT_EQ(id, back);
}

TEST(RequestId, CanonicalDistinguishesStringFromInt) {
    mcp::RequestId i{5};
    mcp::RequestId s{"5"};
    EXPECT_NE(i.canonical(), s.canonical());
    EXPECT_NE(std::hash<mcp::RequestId>{}(i),
              std::hash<mcp::RequestId>{}(s));
}

TEST(RequestId, RejectsBoolAndNull) {
    json j = true;
    mcp::RequestId out;
    EXPECT_THROW(j.get_to(out), mcp::Error);
    j = nullptr;
    EXPECT_THROW(j.get_to(out), mcp::Error);
}

// -------------------------------------------------------------------------
// JSON-RPC envelope
// -------------------------------------------------------------------------

TEST(JsonRpc, RequestRoundTrip) {
    mcp::JsonRpcRequest r{
        .id     = mcp::RequestId{1},
        .method = "tools/list",
        .params = json::object({{"cursor", "abc"}}),
    };
    json j = r;
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["id"],      1);
    EXPECT_EQ(j["method"],  "tools/list");
    EXPECT_EQ(j["params"]["cursor"], "abc");
    auto back = j.get<mcp::JsonRpcRequest>();
    EXPECT_EQ(back.id,     r.id);
    EXPECT_EQ(back.method, r.method);
    EXPECT_EQ(back.params, r.params);
}

TEST(JsonRpc, RequestRejectsBadJsonrpc) {
    json j = {{"jsonrpc", "1.0"}, {"id", 1}, {"method", "x"}};
    EXPECT_THROW(j.get<mcp::JsonRpcRequest>(), mcp::Error);
}

TEST(JsonRpc, NotificationOmitsId) {
    mcp::JsonRpcNotification n{.method = "notifications/initialized"};
    json j = n;
    EXPECT_FALSE(j.contains("id"));
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["method"],  "notifications/initialized");
    auto back = j.get<mcp::JsonRpcNotification>();
    EXPECT_EQ(back.method, n.method);
}

TEST(JsonRpc, ResponseSuccess) {
    mcp::JsonRpcResponse r{
        .id      = mcp::RequestId{1},
        .outcome = json::object({{"k", "v"}}),
    };
    json j = r;
    EXPECT_TRUE(j.contains("result"));
    EXPECT_FALSE(j.contains("error"));
    auto back = j.get<mcp::JsonRpcResponse>();
    ASSERT_TRUE(back.is_success());
    EXPECT_EQ(back.result(), r.result());
}

TEST(JsonRpc, ResponseError) {
    mcp::JsonRpcResponse r{
        .id      = mcp::RequestId{1},
        .outcome = mcp::ErrorObject{mcp::error_code::method_not_found, "no", {}},
    };
    json j = r;
    EXPECT_TRUE(j.contains("error"));
    EXPECT_FALSE(j.contains("result"));
    auto back = j.get<mcp::JsonRpcResponse>();
    ASSERT_TRUE(back.is_error());
    EXPECT_EQ(back.error().code, mcp::error_code::method_not_found);
    EXPECT_EQ(back.error().message, "no");
}

TEST(JsonRpc, ResponseRejectsBothResultAndError) {
    json j = {{"jsonrpc", "2.0"},
              {"id", 1},
              {"result", "x"},
              {"error", {{"code", -1}, {"message", "y"}}}};
    EXPECT_THROW(j.get<mcp::JsonRpcResponse>(), mcp::Error);
}

TEST(JsonRpc, ParseMessageDispatchesOnShape) {
    json req  = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "x"}};
    json note = {{"jsonrpc", "2.0"}, {"method", "x"}};
    json resp = {{"jsonrpc", "2.0"}, {"id", 1}, {"result", "y"}};

    auto m1 = mcp::parse_message(req);
    auto m2 = mcp::parse_message(note);
    auto m3 = mcp::parse_message(resp);

    EXPECT_TRUE(std::holds_alternative<mcp::JsonRpcRequest>(m1));
    EXPECT_TRUE(std::holds_alternative<mcp::JsonRpcNotification>(m2));
    EXPECT_TRUE(std::holds_alternative<mcp::JsonRpcResponse>(m3));
}

TEST(JsonRpc, ParseMessageRejectsBadShape) {
    const json bad = {{"jsonrpc", "2.0"}};
    EXPECT_THROW({ (void)mcp::parse_message(bad); }, mcp::Error);
}

// -------------------------------------------------------------------------
// Implementation, capabilities
// -------------------------------------------------------------------------

TEST(Implementation, RoundTripWithAllFields) {
    mcp::Implementation imp{
        .name        = "calc",
        .title       = "Calculator",
        .version     = "1.0.0",
        .description = "Tiny arithmetic server",
        .website_url = "https://example.com",
    };
    json j = imp;
    EXPECT_EQ(j["name"],        "calc");
    EXPECT_EQ(j["title"],       "Calculator");
    EXPECT_EQ(j["version"],     "1.0.0");
    EXPECT_EQ(j["description"], "Tiny arithmetic server");
    EXPECT_EQ(j["websiteUrl"],  "https://example.com");
    auto back = j.get<mcp::Implementation>();
    EXPECT_EQ(back.name,        imp.name);
    EXPECT_EQ(back.title,       imp.title);
    EXPECT_EQ(back.version,     imp.version);
    EXPECT_EQ(back.description, imp.description);
    EXPECT_EQ(back.website_url, imp.website_url);
}

TEST(Implementation, MinimalShape) {
    mcp::Implementation imp{.name = "x", .version = "1"};
    json j = imp;
    EXPECT_FALSE(j.contains("title"));
    EXPECT_FALSE(j.contains("description"));
    EXPECT_FALSE(j.contains("websiteUrl"));
    auto back = j.get<mcp::Implementation>();
    EXPECT_FALSE(back.title.has_value());
}

TEST(Capabilities, ServerToolsListChanged) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ToolsCapability{.list_changed = true};
    json j = caps;
    EXPECT_EQ(j["tools"]["listChanged"], true);
    auto back = j.get<mcp::ServerCapabilities>();
    ASSERT_TRUE(back.tools.has_value());
    EXPECT_EQ(back.tools->list_changed, true);
}

TEST(Capabilities, ServerLoggingPresenceOnly) {
    mcp::ServerCapabilities caps;
    caps.logging = json::object();  // presence-only
    json j = caps;
    EXPECT_TRUE(j["logging"].is_object());
    auto back = j.get<mcp::ServerCapabilities>();
    EXPECT_TRUE(back.logging.has_value());
}

TEST(Capabilities, ResourcesSubscribeAndListChanged) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ResourcesCapability{
        .subscribe    = true,
        .list_changed = false,
    };
    json j = caps;
    EXPECT_EQ(j["resources"]["subscribe"], true);
    EXPECT_EQ(j["resources"]["listChanged"], false);
    auto back = j.get<mcp::ServerCapabilities>();
    ASSERT_TRUE(back.resources.has_value());
    EXPECT_EQ(back.resources->subscribe,    true);
    EXPECT_EQ(back.resources->list_changed, false);
}

TEST(Capabilities, ClientSamplingTools) {
    mcp::ClientCapabilities caps;
    caps.sampling = mcp::SamplingCapability{.tools = json::object()};
    json j = caps;
    EXPECT_TRUE(j["sampling"]["tools"].is_object());
    auto back = j.get<mcp::ClientCapabilities>();
    ASSERT_TRUE(back.sampling.has_value());
    EXPECT_TRUE(back.sampling->tools.has_value());
}

TEST(Capabilities, EmptyServerCapsSerializeAsEmptyObject) {
    mcp::ServerCapabilities caps;
    json j = caps;
    EXPECT_TRUE(j.is_object());
    EXPECT_TRUE(j.empty());
}

// -------------------------------------------------------------------------
// initialize
// -------------------------------------------------------------------------

TEST(Initialize, RequestParamsRoundTrip) {
    mcp::InitializeRequestParams p{
        .protocol_version = std::string{mcp::kLatestProtocolVersion},
        .capabilities     = {},
        .client_info      = {.name = "c", .version = "1"},
    };
    p.capabilities.roots = mcp::RootsCapability{.list_changed = true};
    json j = p;
    EXPECT_EQ(j["protocolVersion"], "2025-11-25");
    EXPECT_EQ(j["clientInfo"]["name"], "c");
    EXPECT_EQ(j["capabilities"]["roots"]["listChanged"], true);
    auto back = j.get<mcp::InitializeRequestParams>();
    EXPECT_EQ(back.protocol_version, p.protocol_version);
    EXPECT_EQ(back.client_info.name, p.client_info.name);
    ASSERT_TRUE(back.capabilities.roots.has_value());
    EXPECT_EQ(back.capabilities.roots->list_changed, true);
}

TEST(Initialize, ResultRoundTrip) {
    mcp::InitializeResult r{
        .protocol_version = "2025-11-25",
        .capabilities     = {},
        .server_info      = {.name = "s", .version = "1"},
        .instructions     = "hello",
    };
    r.capabilities.tools = mcp::ToolsCapability{.list_changed = true};
    json j = r;
    EXPECT_EQ(j["protocolVersion"],            "2025-11-25");
    EXPECT_EQ(j["serverInfo"]["name"],         "s");
    EXPECT_EQ(j["instructions"],               "hello");
    EXPECT_EQ(j["capabilities"]["tools"]["listChanged"], true);
    auto back = j.get<mcp::InitializeResult>();
    EXPECT_EQ(back.protocol_version, r.protocol_version);
    EXPECT_EQ(back.instructions,     r.instructions);
    ASSERT_TRUE(back.capabilities.tools.has_value());
}

TEST(Initialize, NotificationMethodConstantsAreSpecCorrect) {
    EXPECT_EQ(mcp::method_initialize,                 "initialize");
    EXPECT_EQ(mcp::method_notifications_initialized,  "notifications/initialized");
}

// -------------------------------------------------------------------------
// Content blocks
// -------------------------------------------------------------------------

TEST(Content, TextBlockShape) {
    mcp::TextContent t{.text = "hi"};
    json j = t;
    EXPECT_EQ(j["type"], "text");
    EXPECT_EQ(j["text"], "hi");
    auto back = j.get<mcp::TextContent>();
    EXPECT_EQ(back.text, t.text);
}

TEST(Content, ImageBlockShape) {
    mcp::ImageContent i{.data = "Zm9v", .mime_type = "image/png"};
    json j = i;
    EXPECT_EQ(j["type"],     "image");
    EXPECT_EQ(j["data"],     "Zm9v");
    EXPECT_EQ(j["mimeType"], "image/png");
    auto back = j.get<mcp::ImageContent>();
    EXPECT_EQ(back.mime_type, i.mime_type);
}

TEST(Content, AudioBlockShape) {
    mcp::AudioContent a{.data = "abcd", .mime_type = "audio/wav"};
    json j = a;
    EXPECT_EQ(j["type"],     "audio");
    EXPECT_EQ(j["mimeType"], "audio/wav");
}

TEST(Content, BlockVariantDispatchesOnType) {
    json text = {{"type", "text"}, {"text", "x"}};
    auto block = text.get<mcp::ContentBlock>();
    ASSERT_TRUE(std::holds_alternative<mcp::TextContent>(block));
    EXPECT_EQ(std::get<mcp::TextContent>(block).text, "x");

    json image = {{"type", "image"}, {"data", "Zm9v"}, {"mimeType", "image/png"}};
    block = image.get<mcp::ContentBlock>();
    ASSERT_TRUE(std::holds_alternative<mcp::ImageContent>(block));
}

TEST(Content, BlockVariantRejectsUnknownType) {
    json unknown = {{"type", "video"}};
    EXPECT_THROW(unknown.get<mcp::ContentBlock>(), mcp::Error);
}

TEST(Content, AnnotationsRoundTrip) {
    mcp::Annotations a;
    a.audience      = std::vector<mcp::Role>{mcp::Role::user, mcp::Role::assistant};
    a.priority      = 0.5;
    a.last_modified = "2025-01-12T15:00:58Z";
    json j = a;
    EXPECT_EQ(j["audience"], json::array({"user", "assistant"}));
    EXPECT_EQ(j["priority"], 0.5);
    auto back = j.get<mcp::Annotations>();
    ASSERT_TRUE(back.audience.has_value());
    EXPECT_EQ(back.audience->size(), 2u);
    EXPECT_EQ((*back.audience)[0], mcp::Role::user);
}

// -------------------------------------------------------------------------
// Tools
// -------------------------------------------------------------------------

TEST(Tools, ToolRoundTrip) {
    mcp::Tool t{
        .name         = "add",
        .title        = "Add",
        .description  = "Add two numbers",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"a", {{"type", "number"}}},
                           {"b", {{"type", "number"}}}}},
                         {"required", json::array({"a", "b"})}},
    };
    json j = t;
    EXPECT_EQ(j["name"], "add");
    EXPECT_EQ(j["inputSchema"]["type"], "object");
    auto back = j.get<mcp::Tool>();
    EXPECT_EQ(back.name,        t.name);
    EXPECT_EQ(back.input_schema, t.input_schema);
}

TEST(Tools, ListToolsResultRoundTrip) {
    mcp::ListToolsResult r;
    r.tools.push_back(mcp::Tool{
        .name         = "x",
        .input_schema = {{"type", "object"}},
    });
    r.next_cursor = "next";
    json j = r;
    EXPECT_EQ(j["tools"][0]["name"], "x");
    EXPECT_EQ(j["nextCursor"], "next");
    auto back = j.get<mcp::ListToolsResult>();
    ASSERT_EQ(back.tools.size(), 1u);
    EXPECT_EQ(back.tools[0].name, "x");
    EXPECT_EQ(back.next_cursor, "next");
}

TEST(Tools, CallToolRequestParamsShape) {
    mcp::CallToolRequestParams p{
        .name      = "add",
        .arguments = json{{"a", 1}, {"b", 2}},
    };
    json j = p;
    EXPECT_EQ(j["name"], "add");
    EXPECT_EQ(j["arguments"]["a"], 1);
    auto back = j.get<mcp::CallToolRequestParams>();
    EXPECT_EQ(back.name,      p.name);
    EXPECT_EQ(back.arguments, p.arguments);
}

TEST(Tools, CallToolResultRoundTrip) {
    mcp::CallToolResult r;
    r.content.push_back(mcp::TextContent{.text = "3"});
    r.is_error = false;
    json j = r;
    EXPECT_EQ(j["content"][0]["type"], "text");
    EXPECT_EQ(j["content"][0]["text"], "3");
    EXPECT_EQ(j["isError"], false);
    auto back = j.get<mcp::CallToolResult>();
    ASSERT_EQ(back.content.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<mcp::TextContent>(back.content[0]));
    EXPECT_EQ(std::get<mcp::TextContent>(back.content[0]).text, "3");
}

TEST(Tools, ToolAnnotationsRoundTrip) {
    mcp::ToolAnnotations a{
        .title            = "Add",
        .read_only_hint   = true,
        .destructive_hint = false,
        .idempotent_hint  = true,
        .open_world_hint  = false,
    };
    json j = a;
    EXPECT_EQ(j["readOnlyHint"],    true);
    EXPECT_EQ(j["destructiveHint"], false);
    auto back = j.get<mcp::ToolAnnotations>();
    EXPECT_EQ(back.read_only_hint,   a.read_only_hint);
    EXPECT_EQ(back.destructive_hint, a.destructive_hint);
}

// -------------------------------------------------------------------------
// 2025-11-25 spec additions
// -------------------------------------------------------------------------

TEST(SpecAdditions, ToolUseContentRoundTrip) {
    mcp::ToolUseContent c{
        .id    = "tu_abc",
        .name  = "search",
        .input = json{{"q", "lima beans"}},
    };
    mcp::ContentBlock cb = c;
    json j = cb;
    EXPECT_EQ(j["type"], "tool_use");
    EXPECT_EQ(j["id"],   "tu_abc");
    EXPECT_EQ(j["name"], "search");
    EXPECT_EQ(j["input"]["q"], "lima beans");
    auto back = j.get<mcp::ContentBlock>();
    ASSERT_TRUE(std::holds_alternative<mcp::ToolUseContent>(back));
    EXPECT_EQ(std::get<mcp::ToolUseContent>(back).id, "tu_abc");
}

TEST(SpecAdditions, ToolResultContentRoundTrip) {
    mcp::ToolResultContent c{
        .tool_use_id = "tu_abc",
        .content     = { mcp::TextContent{.text = "found 42"} },
        .is_error    = false,
    };
    mcp::ContentBlock cb = c;
    json j = cb;
    EXPECT_EQ(j["type"],         "tool_result");
    EXPECT_EQ(j["toolUseId"],    "tu_abc");
    EXPECT_EQ(j["content"][0]["type"], "text");
    EXPECT_EQ(j["isError"],      false);
    auto back = j.get<mcp::ContentBlock>();
    ASSERT_TRUE(std::holds_alternative<mcp::ToolResultContent>(back));
    EXPECT_EQ(std::get<mcp::ToolResultContent>(back).tool_use_id, "tu_abc");
}

TEST(SpecAdditions, ToolResultRejectsNestedToolUseInContent) {
    // PlainContentBlock parser MUST reject "tool_use" / "tool_result"
    // — that's the spec rule preventing recursive content blocks.
    json j = json{
        {"type",      "tool_result"},
        {"toolUseId", "x"},
        {"content",   json::array({
            json{{"type", "tool_use"}, {"id", "y"}, {"name", "z"},
                 {"input", json::object()}},
        })},
    };
    EXPECT_THROW((void)j.get<mcp::ContentBlock>(), mcp::Error);
}

TEST(SpecAdditions, SamplingToolsAndToolChoiceRoundTrip) {
    mcp::CreateMessageRequestParams p{
        .messages = { mcp::SamplingMessage{
            .role    = mcp::Role::user,
            .content = { mcp::TextContent{.text = "?"} },
        }},
        .max_tokens = 100,
    };
    p.tools = std::vector<mcp::Tool>{ mcp::Tool{
        .name         = "search",
        .input_schema = json{{"type", "object"}},
    }};
    p.tool_choice = mcp::ToolChoice{ .mode = mcp::ToolChoiceMode::any };

    json j = p;
    ASSERT_TRUE(j.contains("tools"));
    EXPECT_EQ(j["tools"][0]["name"], "search");
    EXPECT_EQ(j["toolChoice"], "any");

    auto back = j.get<mcp::CreateMessageRequestParams>();
    ASSERT_TRUE(back.tools.has_value());
    EXPECT_EQ(back.tools->size(), 1u);
    ASSERT_TRUE(back.tool_choice.has_value());
    EXPECT_EQ(*back.tool_choice->mode, mcp::ToolChoiceMode::any);
}

TEST(SpecAdditions, ToolChoiceObjectFormForNamedTool) {
    mcp::ToolChoice c{
        .mode = mcp::ToolChoiceMode::tool,
        .name = "search",
    };
    json j = c;
    ASSERT_TRUE(j.is_object());
    EXPECT_EQ(j["type"], "tool");
    EXPECT_EQ(j["name"], "search");
    auto back = j.get<mcp::ToolChoice>();
    EXPECT_EQ(*back.mode, mcp::ToolChoiceMode::tool);
    EXPECT_EQ(*back.name, "search");
}

TEST(SpecAdditions, IconsAndMetaOnImplementation) {
    mcp::Implementation impl{
        .name    = "x", .version = "1",
        .icons   = std::vector<mcp::Icon>{{
            .src = "https://example.com/icon.png", .mime_type = "image/png",
        }},
        .meta    = json{{"app.example/internal", 42}},
    };
    json j = impl;
    EXPECT_EQ(j["icons"][0]["src"], "https://example.com/icon.png");
    EXPECT_EQ(j["_meta"]["app.example/internal"], 42);
    auto back = j.get<mcp::Implementation>();
    ASSERT_TRUE(back.icons.has_value());
    EXPECT_EQ(back.icons->front().src, "https://example.com/icon.png");
    ASSERT_TRUE(back.meta.has_value());
    EXPECT_EQ((*back.meta)["app.example/internal"], 42);
}

TEST(SpecAdditions, MetaOnInitializeResult) {
    mcp::InitializeResult r{
        .protocol_version = "2025-11-25",
        .capabilities     = {},
        .server_info      = mcp::Implementation{.name = "s", .version = "0"},
        .meta             = json{{"x", 1}},
    };
    json j = r;
    EXPECT_EQ(j["_meta"]["x"], 1);
    auto back = j.get<mcp::InitializeResult>();
    ASSERT_TRUE(back.meta.has_value());
    EXPECT_EQ((*back.meta)["x"], 1);
}

TEST(SpecAdditions, ToolExecutionRoundTrip) {
    mcp::Tool t{
        .name         = "danger",
        .input_schema = json{{"type", "object"}},
        .execution    = mcp::ToolExecution{
            .task_support = mcp::TaskSupport::required,
        },
    };
    json j = t;
    EXPECT_EQ(j["execution"]["taskSupport"], "required");
    auto back = j.get<mcp::Tool>();
    ASSERT_TRUE(back.execution.has_value());
    EXPECT_EQ(back.execution->task_support, mcp::TaskSupport::required);
}

TEST(SpecAdditions, UrlElicitationRequiredErrorDataRoundTrip) {
    mcp::UrlElicitationRequiredErrorData d{
        .elicitations = { mcp::ElicitUrlRequestParams{
            .message        = "Authorise via the linked URL",
            .url            = "https://idp.example/auth",
            .elicitation_id = "eid-1",
        }},
    };
    json j = d;
    EXPECT_EQ(j["elicitations"][0]["url"], "https://idp.example/auth");
    auto back = j.get<mcp::UrlElicitationRequiredErrorData>();
    EXPECT_EQ(back.elicitations.front().elicitation_id, "eid-1");
}

}  // namespace
