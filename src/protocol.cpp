// SPDX-License-Identifier: Apache-2.0
#include "mcp/protocol.hpp"

#include "mcp/error.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace mcp {

// =====================================================================
// Helpers
// =====================================================================
namespace {

// Pull a required field. Throws Error(parse_error) if absent.
template <typename T>
T require(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) {
        throw Error(error_code::parse_error,
                    std::string{"missing required field: "} + key);
    }
    return it->get<T>();
}

// Pull an optional field. Sets `out` only when key is present and not null.
template <typename T>
void take_optional(const nlohmann::json& j, const char* key,
                   std::optional<T>& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    out = it->get<T>();
}

// Like take_optional, but reads a raw JSON value (used for `experimental`
// blobs and presence-only `{}` fields).
inline void take_optional_json(const nlohmann::json& j, const char* key,
                               std::optional<nlohmann::json>& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    out = *it;
}

// If `value` is engaged, write it under `key`.
template <typename T>
void put_optional(nlohmann::json& j, const char* key,
                  const std::optional<T>& value) {
    if (value.has_value()) j[key] = *value;
}

}  // namespace

// =====================================================================
// RequestId
// =====================================================================

std::string RequestId::canonical() const {
    if (is_string()) {
        return "s:" + as_string();
    }
    return "i:" + std::to_string(as_integer());
}

void to_json(nlohmann::json& j, const RequestId& id) {
    std::visit([&j](const auto& v) { j = v; }, id.value());
}

void from_json(const nlohmann::json& j, RequestId& id) {
    if (j.is_string()) {
        id = RequestId{j.get<std::string>()};
    } else if (j.is_number_integer()) {
        id = RequestId{j.get<std::int64_t>()};
    } else {
        throw Error(error_code::parse_error,
                    "JSON-RPC id must be a string or integer");
    }
}

// =====================================================================
// JSON-RPC envelopes
// =====================================================================

void to_json(nlohmann::json& j, const JsonRpcRequest& r) {
    j = nlohmann::json::object();
    j["jsonrpc"] = kJsonrpcVersion;
    j["id"]      = r.id;
    j["method"]  = r.method;
    if (r.params.has_value()) j["params"] = *r.params;
}

void from_json(const nlohmann::json& j, JsonRpcRequest& r) {
    if (require<std::string>(j, "jsonrpc") != kJsonrpcVersion) {
        throw Error(error_code::invalid_request, "jsonrpc must be \"2.0\"");
    }
    r.id     = j.at("id").get<RequestId>();
    r.method = require<std::string>(j, "method");
    take_optional_json(j, "params", r.params);
}

void to_json(nlohmann::json& j, const JsonRpcNotification& n) {
    j = nlohmann::json::object();
    j["jsonrpc"] = kJsonrpcVersion;
    j["method"]  = n.method;
    if (n.params.has_value()) j["params"] = *n.params;
}

void from_json(const nlohmann::json& j, JsonRpcNotification& n) {
    if (require<std::string>(j, "jsonrpc") != kJsonrpcVersion) {
        throw Error(error_code::invalid_request, "jsonrpc must be \"2.0\"");
    }
    n.method = require<std::string>(j, "method");
    take_optional_json(j, "params", n.params);
}

void to_json(nlohmann::json& j, const ErrorObject& e) {
    j = nlohmann::json{{"code", e.code}, {"message", e.message}};
    if (e.has_data()) j["data"] = e.data;
}

void from_json(const nlohmann::json& j, ErrorObject& e) {
    e.code    = require<int>(j, "code");
    e.message = require<std::string>(j, "message");
    if (auto it = j.find("data"); it != j.end()) e.data = *it;
    else                                          e.data = nullptr;
}

void to_json(nlohmann::json& j, const JsonRpcResponse& r) {
    j = nlohmann::json::object();
    j["jsonrpc"] = kJsonrpcVersion;
    if (r.id.has_value()) j["id"] = *r.id;
    else                  j["id"] = nullptr;
    if (r.is_success()) j["result"] = std::get<0>(r.outcome);
    else                j["error"]  = std::get<1>(r.outcome);
}

void from_json(const nlohmann::json& j, JsonRpcResponse& r) {
    if (require<std::string>(j, "jsonrpc") != kJsonrpcVersion) {
        throw Error(error_code::invalid_request, "jsonrpc must be \"2.0\"");
    }
    if (auto it = j.find("id"); it != j.end() && !it->is_null()) {
        r.id = it->get<RequestId>();
    } else {
        r.id.reset();
    }
    const bool has_result = j.contains("result");
    const bool has_error  = j.contains("error");
    if (has_result == has_error) {
        throw Error(error_code::invalid_request,
                    "JSON-RPC response must carry exactly one of "
                    "\"result\" or \"error\"");
    }
    if (has_result) {
        r.outcome = j.at("result");
    } else {
        r.outcome = j.at("error").get<ErrorObject>();
    }
}

JsonRpcMessage parse_message(const nlohmann::json& j) {
    if (!j.is_object()) {
        throw Error(error_code::invalid_request,
                    "JSON-RPC frame must be an object");
    }
    const bool has_id     = j.contains("id");
    const bool has_method = j.contains("method");
    const bool has_result = j.contains("result");
    const bool has_error  = j.contains("error");

    if (has_method && has_id)  return j.get<JsonRpcRequest>();
    if (has_method)            return j.get<JsonRpcNotification>();
    if (has_result || has_error) return j.get<JsonRpcResponse>();

    throw Error(error_code::invalid_request,
                "JSON-RPC frame has neither \"method\" nor \"result\"/\"error\"");
}

nlohmann::json serialize_message(const JsonRpcMessage& m) {
    nlohmann::json j;
    std::visit([&j](const auto& msg) { j = msg; }, m);
    return j;
}

// =====================================================================
// Implementation
// =====================================================================

void to_json(nlohmann::json& j, const Implementation& i) {
    j = nlohmann::json::object();
    j["name"]    = i.name;
    j["version"] = i.version;
    put_optional(j, "title",       i.title);
    put_optional(j, "description", i.description);
    put_optional(j, "websiteUrl",  i.website_url);
}

void from_json(const nlohmann::json& j, Implementation& i) {
    i.name    = require<std::string>(j, "name");
    i.version = require<std::string>(j, "version");
    take_optional(j, "title",       i.title);
    take_optional(j, "description", i.description);
    take_optional(j, "websiteUrl",  i.website_url);
}

// =====================================================================
// Capabilities
// =====================================================================

void to_json(nlohmann::json& j, const RootsCapability& c) {
    j = nlohmann::json::object();
    put_optional(j, "listChanged", c.list_changed);
}

void from_json(const nlohmann::json& j, RootsCapability& c) {
    take_optional(j, "listChanged", c.list_changed);
}

void to_json(nlohmann::json& j, const SamplingCapability& c) {
    j = nlohmann::json::object();
    put_optional(j, "context", c.context);
    put_optional(j, "tools",   c.tools);
}

void from_json(const nlohmann::json& j, SamplingCapability& c) {
    take_optional_json(j, "context", c.context);
    take_optional_json(j, "tools",   c.tools);
}

void to_json(nlohmann::json& j, const ElicitationCapability& c) {
    j = nlohmann::json::object();
    put_optional(j, "form", c.form);
    put_optional(j, "url",  c.url);
}

void from_json(const nlohmann::json& j, ElicitationCapability& c) {
    take_optional_json(j, "form", c.form);
    take_optional_json(j, "url",  c.url);
}

void to_json(nlohmann::json& j, const ClientCapabilities& c) {
    j = nlohmann::json::object();
    put_optional(j, "experimental", c.experimental);
    put_optional(j, "roots",        c.roots);
    put_optional(j, "sampling",     c.sampling);
    put_optional(j, "elicitation",  c.elicitation);
}

void from_json(const nlohmann::json& j, ClientCapabilities& c) {
    take_optional_json(j, "experimental", c.experimental);
    take_optional      (j, "roots",        c.roots);
    take_optional      (j, "sampling",     c.sampling);
    take_optional      (j, "elicitation",  c.elicitation);
}

void to_json(nlohmann::json& j, const PromptsCapability& c) {
    j = nlohmann::json::object();
    put_optional(j, "listChanged", c.list_changed);
}
void from_json(const nlohmann::json& j, PromptsCapability& c) {
    take_optional(j, "listChanged", c.list_changed);
}

void to_json(nlohmann::json& j, const ResourcesCapability& c) {
    j = nlohmann::json::object();
    put_optional(j, "subscribe",   c.subscribe);
    put_optional(j, "listChanged", c.list_changed);
}
void from_json(const nlohmann::json& j, ResourcesCapability& c) {
    take_optional(j, "subscribe",   c.subscribe);
    take_optional(j, "listChanged", c.list_changed);
}

void to_json(nlohmann::json& j, const ToolsCapability& c) {
    j = nlohmann::json::object();
    put_optional(j, "listChanged", c.list_changed);
}
void from_json(const nlohmann::json& j, ToolsCapability& c) {
    take_optional(j, "listChanged", c.list_changed);
}

void to_json(nlohmann::json& j, const ServerCapabilities& c) {
    j = nlohmann::json::object();
    put_optional(j, "experimental", c.experimental);
    put_optional(j, "logging",      c.logging);
    put_optional(j, "completions",  c.completions);
    put_optional(j, "prompts",      c.prompts);
    put_optional(j, "resources",    c.resources);
    put_optional(j, "tools",        c.tools);
}

void from_json(const nlohmann::json& j, ServerCapabilities& c) {
    take_optional_json(j, "experimental", c.experimental);
    take_optional_json(j, "logging",      c.logging);
    take_optional_json(j, "completions",  c.completions);
    take_optional      (j, "prompts",      c.prompts);
    take_optional      (j, "resources",    c.resources);
    take_optional      (j, "tools",        c.tools);
}

// =====================================================================
// initialize
// =====================================================================

void to_json(nlohmann::json& j, const InitializeRequestParams& p) {
    j = nlohmann::json::object();
    j["protocolVersion"] = p.protocol_version;
    j["capabilities"]    = p.capabilities;
    j["clientInfo"]      = p.client_info;
}

void from_json(const nlohmann::json& j, InitializeRequestParams& p) {
    p.protocol_version = require<std::string>(j, "protocolVersion");
    p.capabilities     = j.at("capabilities").get<ClientCapabilities>();
    p.client_info      = j.at("clientInfo").get<Implementation>();
}

void to_json(nlohmann::json& j, const InitializeResult& r) {
    j = nlohmann::json::object();
    j["protocolVersion"] = r.protocol_version;
    j["capabilities"]    = r.capabilities;
    j["serverInfo"]      = r.server_info;
    put_optional(j, "instructions", r.instructions);
}

void from_json(const nlohmann::json& j, InitializeResult& r) {
    r.protocol_version = require<std::string>(j, "protocolVersion");
    r.capabilities     = j.at("capabilities").get<ServerCapabilities>();
    r.server_info      = j.at("serverInfo").get<Implementation>();
    take_optional(j, "instructions", r.instructions);
}

// =====================================================================
// Content blocks
// =====================================================================

std::string_view to_string(Role r) noexcept {
    switch (r) {
        case Role::user:      return "user";
        case Role::assistant: return "assistant";
    }
    return "?";
}

namespace {

Role role_from_string(std::string_view s) {
    if (s == "user")      return Role::user;
    if (s == "assistant") return Role::assistant;
    throw Error(error_code::parse_error,
                std::string{"unknown role: "} + std::string{s});
}

}  // namespace

// nlohmann::json doesn't auto-serialize enums; provide explicit hooks here
// rather than using NLOHMANN_JSON_SERIALIZE_ENUM (cleaner error handling).
void to_json(nlohmann::json& j, const Role& r)   { j = std::string{to_string(r)}; }
void from_json(const nlohmann::json& j, Role& r) { r = role_from_string(j.get<std::string>()); }

void to_json(nlohmann::json& j, const Annotations& a) {
    j = nlohmann::json::object();
    if (a.audience.has_value()) {
        nlohmann::json arr = nlohmann::json::array();
        for (auto r : *a.audience) arr.push_back(r);
        j["audience"] = std::move(arr);
    }
    put_optional(j, "priority",     a.priority);
    put_optional(j, "lastModified", a.last_modified);
}

void from_json(const nlohmann::json& j, Annotations& a) {
    if (auto it = j.find("audience"); it != j.end() && it->is_array()) {
        std::vector<Role> roles;
        roles.reserve(it->size());
        for (const auto& v : *it) roles.push_back(v.get<Role>());
        a.audience = std::move(roles);
    }
    take_optional(j, "priority",     a.priority);
    take_optional(j, "lastModified", a.last_modified);
}

void to_json(nlohmann::json& j, const TextContent& c) {
    j = nlohmann::json{{"type", "text"}, {"text", c.text}};
    put_optional(j, "annotations", c.annotations);
}

void from_json(const nlohmann::json& j, TextContent& c) {
    c.text = require<std::string>(j, "text");
    take_optional(j, "annotations", c.annotations);
}

void to_json(nlohmann::json& j, const ImageContent& c) {
    j = nlohmann::json{{"type",     "image"},
                       {"data",     c.data},
                       {"mimeType", c.mime_type}};
    put_optional(j, "annotations", c.annotations);
}

void from_json(const nlohmann::json& j, ImageContent& c) {
    c.data      = require<std::string>(j, "data");
    c.mime_type = require<std::string>(j, "mimeType");
    take_optional(j, "annotations", c.annotations);
}

void to_json(nlohmann::json& j, const AudioContent& c) {
    j = nlohmann::json{{"type",     "audio"},
                       {"data",     c.data},
                       {"mimeType", c.mime_type}};
    put_optional(j, "annotations", c.annotations);
}

void from_json(const nlohmann::json& j, AudioContent& c) {
    c.data      = require<std::string>(j, "data");
    c.mime_type = require<std::string>(j, "mimeType");
    take_optional(j, "annotations", c.annotations);
}

// =====================================================================
// Resources
// =====================================================================

void to_json(nlohmann::json& j, const TextResourceContents& c) {
    j = nlohmann::json::object();
    j["uri"]  = c.uri;
    j["text"] = c.text;
    put_optional(j, "mimeType", c.mime_type);
}

void from_json(const nlohmann::json& j, TextResourceContents& c) {
    c.uri  = require<std::string>(j, "uri");
    c.text = require<std::string>(j, "text");
    take_optional(j, "mimeType", c.mime_type);
}

void to_json(nlohmann::json& j, const BlobResourceContents& c) {
    j = nlohmann::json::object();
    j["uri"]  = c.uri;
    j["blob"] = c.blob;
    put_optional(j, "mimeType", c.mime_type);
}

void from_json(const nlohmann::json& j, BlobResourceContents& c) {
    c.uri  = require<std::string>(j, "uri");
    c.blob = require<std::string>(j, "blob");
    take_optional(j, "mimeType", c.mime_type);
}

void to_json(nlohmann::json& j, const ResourceContents& c) {
    std::visit([&j](const auto& v) { j = v; }, c);
}

void from_json(const nlohmann::json& j, ResourceContents& c) {
    if (j.contains("text")) { c = j.get<TextResourceContents>(); return; }
    if (j.contains("blob")) { c = j.get<BlobResourceContents>(); return; }
    throw Error(error_code::parse_error,
                "ResourceContents must have either \"text\" or \"blob\"");
}

void to_json(nlohmann::json& j, const Resource& r) {
    j = nlohmann::json::object();
    j["uri"]  = r.uri;
    j["name"] = r.name;
    put_optional(j, "title",       r.title);
    put_optional(j, "description", r.description);
    put_optional(j, "mimeType",    r.mime_type);
    put_optional(j, "annotations", r.annotations);
    put_optional(j, "size",        r.size);
}

void from_json(const nlohmann::json& j, Resource& r) {
    r.uri  = require<std::string>(j, "uri");
    r.name = require<std::string>(j, "name");
    take_optional(j, "title",       r.title);
    take_optional(j, "description", r.description);
    take_optional(j, "mimeType",    r.mime_type);
    take_optional(j, "annotations", r.annotations);
    take_optional(j, "size",        r.size);
}

void to_json(nlohmann::json& j, const ResourceTemplate& r) {
    j = nlohmann::json::object();
    j["uriTemplate"] = r.uri_template;
    j["name"]        = r.name;
    put_optional(j, "title",       r.title);
    put_optional(j, "description", r.description);
    put_optional(j, "mimeType",    r.mime_type);
    put_optional(j, "annotations", r.annotations);
}

void from_json(const nlohmann::json& j, ResourceTemplate& r) {
    r.uri_template = require<std::string>(j, "uriTemplate");
    r.name         = require<std::string>(j, "name");
    take_optional(j, "title",       r.title);
    take_optional(j, "description", r.description);
    take_optional(j, "mimeType",    r.mime_type);
    take_optional(j, "annotations", r.annotations);
}

void to_json(nlohmann::json& j, const ResourceLink& r) {
    j = nlohmann::json::object();
    j["type"] = "resource_link";
    j["uri"]  = r.uri;
    j["name"] = r.name;
    put_optional(j, "title",       r.title);
    put_optional(j, "description", r.description);
    put_optional(j, "mimeType",    r.mime_type);
    put_optional(j, "annotations", r.annotations);
    put_optional(j, "size",        r.size);
}

void from_json(const nlohmann::json& j, ResourceLink& r) {
    r.uri  = require<std::string>(j, "uri");
    r.name = require<std::string>(j, "name");
    take_optional(j, "title",       r.title);
    take_optional(j, "description", r.description);
    take_optional(j, "mimeType",    r.mime_type);
    take_optional(j, "annotations", r.annotations);
    take_optional(j, "size",        r.size);
}

void to_json(nlohmann::json& j, const EmbeddedResource& r) {
    j = nlohmann::json::object();
    j["type"]     = "resource";
    j["resource"] = r.resource;
    put_optional(j, "annotations", r.annotations);
}

void from_json(const nlohmann::json& j, EmbeddedResource& r) {
    r.resource = j.at("resource").get<ResourceContents>();
    take_optional(j, "annotations", r.annotations);
}

void to_json(nlohmann::json& j, const ContentBlock& c) {
    std::visit([&j](const auto& v) { j = v; }, c);
}

void from_json(const nlohmann::json& j, ContentBlock& c) {
    const auto t = require<std::string>(j, "type");
    if (t == "text")          { c = j.get<TextContent>();      return; }
    if (t == "image")         { c = j.get<ImageContent>();     return; }
    if (t == "audio")         { c = j.get<AudioContent>();     return; }
    if (t == "resource_link") { c = j.get<ResourceLink>();     return; }
    if (t == "resource")      { c = j.get<EmbeddedResource>(); return; }
    throw Error(error_code::parse_error,
                std::string{"unknown content block type: "} + t);
}

// =====================================================================
// Tools
// =====================================================================

void to_json(nlohmann::json& j, const ToolAnnotations& a) {
    j = nlohmann::json::object();
    put_optional(j, "title",            a.title);
    put_optional(j, "readOnlyHint",     a.read_only_hint);
    put_optional(j, "destructiveHint",  a.destructive_hint);
    put_optional(j, "idempotentHint",   a.idempotent_hint);
    put_optional(j, "openWorldHint",    a.open_world_hint);
}

void from_json(const nlohmann::json& j, ToolAnnotations& a) {
    take_optional(j, "title",            a.title);
    take_optional(j, "readOnlyHint",     a.read_only_hint);
    take_optional(j, "destructiveHint",  a.destructive_hint);
    take_optional(j, "idempotentHint",   a.idempotent_hint);
    take_optional(j, "openWorldHint",    a.open_world_hint);
}

void to_json(nlohmann::json& j, const Tool& t) {
    j = nlohmann::json::object();
    j["name"]        = t.name;
    j["inputSchema"] = t.input_schema;
    put_optional(j, "title",        t.title);
    put_optional(j, "description",  t.description);
    put_optional(j, "outputSchema", t.output_schema);
    put_optional(j, "annotations",  t.annotations);
}

void from_json(const nlohmann::json& j, Tool& t) {
    t.name         = require<std::string>(j, "name");
    t.input_schema = require<nlohmann::json>(j, "inputSchema");
    take_optional      (j, "title",        t.title);
    take_optional      (j, "description",  t.description);
    take_optional_json (j, "outputSchema", t.output_schema);
    take_optional      (j, "annotations",  t.annotations);
}

void to_json(nlohmann::json& j, const ListToolsRequestParams& p) {
    j = nlohmann::json::object();
    put_optional(j, "cursor", p.cursor);
}

void from_json(const nlohmann::json& j, ListToolsRequestParams& p) {
    take_optional(j, "cursor", p.cursor);
}

void to_json(nlohmann::json& j, const ListToolsResult& r) {
    j = nlohmann::json::object();
    j["tools"] = r.tools;
    put_optional(j, "nextCursor", r.next_cursor);
}

void from_json(const nlohmann::json& j, ListToolsResult& r) {
    r.tools = j.at("tools").get<std::vector<Tool>>();
    take_optional(j, "nextCursor", r.next_cursor);
}

void to_json(nlohmann::json& j, const CallToolRequestParams& p) {
    j = nlohmann::json::object();
    j["name"] = p.name;
    put_optional(j, "arguments", p.arguments);
}

void from_json(const nlohmann::json& j, CallToolRequestParams& p) {
    p.name = require<std::string>(j, "name");
    take_optional_json(j, "arguments", p.arguments);
}

void to_json(nlohmann::json& j, const CallToolResult& r) {
    j = nlohmann::json::object();
    j["content"] = r.content;
    put_optional(j, "structuredContent", r.structured_content);
    put_optional(j, "isError",           r.is_error);
}

void from_json(const nlohmann::json& j, CallToolResult& r) {
    r.content = j.at("content").get<std::vector<ContentBlock>>();
    take_optional_json(j, "structuredContent", r.structured_content);
    take_optional      (j, "isError",           r.is_error);
}

// =====================================================================
// Resource requests / responses
// =====================================================================

void to_json(nlohmann::json& j, const ListResourcesRequestParams& p) {
    j = nlohmann::json::object();
    put_optional(j, "cursor", p.cursor);
}
void from_json(const nlohmann::json& j, ListResourcesRequestParams& p) {
    take_optional(j, "cursor", p.cursor);
}

void to_json(nlohmann::json& j, const ListResourcesResult& r) {
    j = nlohmann::json::object();
    j["resources"] = r.resources;
    put_optional(j, "nextCursor", r.next_cursor);
}
void from_json(const nlohmann::json& j, ListResourcesResult& r) {
    r.resources = j.at("resources").get<std::vector<Resource>>();
    take_optional(j, "nextCursor", r.next_cursor);
}

void to_json(nlohmann::json& j, const ListResourceTemplatesRequestParams& p) {
    j = nlohmann::json::object();
    put_optional(j, "cursor", p.cursor);
}
void from_json(const nlohmann::json& j, ListResourceTemplatesRequestParams& p) {
    take_optional(j, "cursor", p.cursor);
}

void to_json(nlohmann::json& j, const ListResourceTemplatesResult& r) {
    j = nlohmann::json::object();
    j["resourceTemplates"] = r.resource_templates;
    put_optional(j, "nextCursor", r.next_cursor);
}
void from_json(const nlohmann::json& j, ListResourceTemplatesResult& r) {
    r.resource_templates = j.at("resourceTemplates").get<std::vector<ResourceTemplate>>();
    take_optional(j, "nextCursor", r.next_cursor);
}

void to_json(nlohmann::json& j, const ReadResourceRequestParams& p) {
    j = nlohmann::json{{"uri", p.uri}};
}
void from_json(const nlohmann::json& j, ReadResourceRequestParams& p) {
    p.uri = require<std::string>(j, "uri");
}

void to_json(nlohmann::json& j, const ReadResourceResult& r) {
    j = nlohmann::json::object();
    j["contents"] = r.contents;
}
void from_json(const nlohmann::json& j, ReadResourceResult& r) {
    r.contents = j.at("contents").get<std::vector<ResourceContents>>();
}

void to_json(nlohmann::json& j, const SubscribeRequestParams& p) {
    j = nlohmann::json{{"uri", p.uri}};
}
void from_json(const nlohmann::json& j, SubscribeRequestParams& p) {
    p.uri = require<std::string>(j, "uri");
}

void to_json(nlohmann::json& j, const UnsubscribeRequestParams& p) {
    j = nlohmann::json{{"uri", p.uri}};
}
void from_json(const nlohmann::json& j, UnsubscribeRequestParams& p) {
    p.uri = require<std::string>(j, "uri");
}

void to_json(nlohmann::json& j, const ResourceUpdatedNotificationParams& p) {
    j = nlohmann::json{{"uri", p.uri}};
}
void from_json(const nlohmann::json& j, ResourceUpdatedNotificationParams& p) {
    p.uri = require<std::string>(j, "uri");
}

// =====================================================================
// Prompts
// =====================================================================

void to_json(nlohmann::json& j, const PromptArgument& a) {
    j = nlohmann::json::object();
    j["name"] = a.name;
    put_optional(j, "title",       a.title);
    put_optional(j, "description", a.description);
    put_optional(j, "required",    a.required);
}
void from_json(const nlohmann::json& j, PromptArgument& a) {
    a.name = require<std::string>(j, "name");
    take_optional(j, "title",       a.title);
    take_optional(j, "description", a.description);
    take_optional(j, "required",    a.required);
}

void to_json(nlohmann::json& j, const Prompt& p) {
    j = nlohmann::json::object();
    j["name"] = p.name;
    put_optional(j, "title",       p.title);
    put_optional(j, "description", p.description);
    if (p.arguments.has_value()) j["arguments"] = *p.arguments;
}
void from_json(const nlohmann::json& j, Prompt& p) {
    p.name = require<std::string>(j, "name");
    take_optional(j, "title",       p.title);
    take_optional(j, "description", p.description);
    if (auto it = j.find("arguments"); it != j.end() && !it->is_null()) {
        p.arguments = it->get<std::vector<PromptArgument>>();
    }
}

void to_json(nlohmann::json& j, const PromptMessage& m) {
    j = nlohmann::json::object();
    j["role"]    = m.role;
    j["content"] = m.content;
}
void from_json(const nlohmann::json& j, PromptMessage& m) {
    m.role    = j.at("role").get<Role>();
    m.content = j.at("content").get<ContentBlock>();
}

void to_json(nlohmann::json& j, const ListPromptsRequestParams& p) {
    j = nlohmann::json::object();
    put_optional(j, "cursor", p.cursor);
}
void from_json(const nlohmann::json& j, ListPromptsRequestParams& p) {
    take_optional(j, "cursor", p.cursor);
}

void to_json(nlohmann::json& j, const ListPromptsResult& r) {
    j = nlohmann::json::object();
    j["prompts"] = r.prompts;
    put_optional(j, "nextCursor", r.next_cursor);
}
void from_json(const nlohmann::json& j, ListPromptsResult& r) {
    r.prompts = j.at("prompts").get<std::vector<Prompt>>();
    take_optional(j, "nextCursor", r.next_cursor);
}

void to_json(nlohmann::json& j, const GetPromptRequestParams& p) {
    j = nlohmann::json::object();
    j["name"] = p.name;
    if (p.arguments.has_value()) {
        nlohmann::json args = nlohmann::json::object();
        for (const auto& [k, v] : *p.arguments) args[k] = v;
        j["arguments"] = std::move(args);
    }
}
void from_json(const nlohmann::json& j, GetPromptRequestParams& p) {
    p.name = require<std::string>(j, "name");
    if (auto it = j.find("arguments"); it != j.end() && it->is_object()) {
        std::unordered_map<std::string, std::string> args;
        for (auto i = it->begin(); i != it->end(); ++i) {
            if (i.value().is_string()) args.emplace(i.key(), i.value().get<std::string>());
        }
        p.arguments = std::move(args);
    }
}

void to_json(nlohmann::json& j, const GetPromptResult& r) {
    j = nlohmann::json::object();
    put_optional(j, "description", r.description);
    j["messages"] = r.messages;
}
void from_json(const nlohmann::json& j, GetPromptResult& r) {
    take_optional(j, "description", r.description);
    r.messages = j.at("messages").get<std::vector<PromptMessage>>();
}

// =====================================================================
// Cancellation
// =====================================================================

void to_json(nlohmann::json& j, const CancelledNotificationParams& p) {
    j = nlohmann::json::object();
    if (p.request_id.has_value()) j["requestId"] = *p.request_id;
    put_optional(j, "reason", p.reason);
}

void from_json(const nlohmann::json& j, CancelledNotificationParams& p) {
    if (auto it = j.find("requestId"); it != j.end() && !it->is_null()) {
        p.request_id = it->get<RequestId>();
    }
    take_optional(j, "reason", p.reason);
}

// =====================================================================
// Progress
// =====================================================================

std::string ProgressToken::canonical() const {
    return is_string() ? "s:" + as_string()
                       : "i:" + std::to_string(as_integer());
}

void to_json(nlohmann::json& j, const ProgressToken& t) {
    if (t.is_string()) j = t.as_string();
    else                j = t.as_integer();
}
void from_json(const nlohmann::json& j, ProgressToken& t) {
    if (j.is_string())              t = ProgressToken{j.get<std::string>()};
    else if (j.is_number_integer()) t = ProgressToken{j.get<std::int64_t>()};
    else throw Error(error_code::parse_error,
                     "progressToken must be a string or integer");
}

void to_json(nlohmann::json& j, const ProgressNotificationParams& p) {
    j = nlohmann::json::object();
    j["progressToken"] = p.progress_token;
    j["progress"]      = p.progress;
    put_optional(j, "total",   p.total);
    put_optional(j, "message", p.message);
}
void from_json(const nlohmann::json& j, ProgressNotificationParams& p) {
    p.progress_token = j.at("progressToken").get<ProgressToken>();
    p.progress       = j.at("progress").get<double>();
    take_optional(j, "total",   p.total);
    take_optional(j, "message", p.message);
}

// =====================================================================
// Logging (protocol-level)
// =====================================================================

std::string_view to_string(LoggingLevel l) noexcept {
    switch (l) {
        case LoggingLevel::debug:     return "debug";
        case LoggingLevel::info:      return "info";
        case LoggingLevel::notice:    return "notice";
        case LoggingLevel::warning:   return "warning";
        case LoggingLevel::error:     return "error";
        case LoggingLevel::critical:  return "critical";
        case LoggingLevel::alert:     return "alert";
        case LoggingLevel::emergency: return "emergency";
    }
    return "?";
}

namespace {
LoggingLevel logging_level_from_string(std::string_view s) {
    if (s == "debug")     return LoggingLevel::debug;
    if (s == "info")      return LoggingLevel::info;
    if (s == "notice")    return LoggingLevel::notice;
    if (s == "warning")   return LoggingLevel::warning;
    if (s == "error")     return LoggingLevel::error;
    if (s == "critical")  return LoggingLevel::critical;
    if (s == "alert")     return LoggingLevel::alert;
    if (s == "emergency") return LoggingLevel::emergency;
    throw Error(error_code::parse_error,
                std::string{"unknown logging level: "} + std::string{s});
}
}  // namespace

void to_json(nlohmann::json& j, const LoggingLevel& l)   { j = std::string{to_string(l)}; }
void from_json(const nlohmann::json& j, LoggingLevel& l) {
    l = logging_level_from_string(j.get<std::string>());
}

void to_json(nlohmann::json& j, const SetLevelRequestParams& p) {
    j = nlohmann::json{{"level", p.level}};
}
void from_json(const nlohmann::json& j, SetLevelRequestParams& p) {
    p.level = j.at("level").get<LoggingLevel>();
}

void to_json(nlohmann::json& j, const LoggingMessageNotificationParams& p) {
    j = nlohmann::json::object();
    j["level"] = p.level;
    j["data"]  = p.data;
    put_optional(j, "logger", p.logger);
}
void from_json(const nlohmann::json& j, LoggingMessageNotificationParams& p) {
    p.level = j.at("level").get<LoggingLevel>();
    p.data  = j.at("data");
    take_optional(j, "logger", p.logger);
}

}  // namespace mcp
