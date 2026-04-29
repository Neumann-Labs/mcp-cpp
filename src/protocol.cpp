// SPDX-License-Identifier: Apache-2.0
#include "mcp/protocol.hpp"

#include "mcp/error.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
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
    } else if (j.is_number_float()) {
        // The spec types `id` as `string | number`; some JS clients
        // emit small whole numbers as floats (`1.0`) because JS has
        // no integer literal. Accept whole floats; reject fractional.
        const double v = j.get<double>();
        const auto truncated = static_cast<std::int64_t>(v);
        if (static_cast<double>(truncated) != v) {
            throw Error(error_code::parse_error,
                        "JSON-RPC id must be an integer (got fractional number)");
        }
        id = RequestId{truncated};
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

void to_json(nlohmann::json& j, const Icon& c) {
    j = nlohmann::json::object();
    j["src"] = c.src;
    put_optional(j, "mimeType", c.mime_type);
    put_optional(j, "sizes",    c.sizes);
    put_optional(j, "theme",    c.theme);
}
void from_json(const nlohmann::json& j, Icon& c) {
    c.src = require<std::string>(j, "src");
    take_optional(j, "mimeType", c.mime_type);
    take_optional(j, "sizes",    c.sizes);
    take_optional(j, "theme",    c.theme);
}

void to_json(nlohmann::json& j, const Implementation& i) {
    j = nlohmann::json::object();
    j["name"]    = i.name;
    j["version"] = i.version;
    put_optional(j, "title",       i.title);
    put_optional(j, "description", i.description);
    put_optional(j, "websiteUrl",  i.website_url);
    put_optional(j, "icons",       i.icons);
    put_optional(j, "_meta",       i.meta);
}

void from_json(const nlohmann::json& j, Implementation& i) {
    i.name    = require<std::string>(j, "name");
    i.version = require<std::string>(j, "version");
    take_optional(j, "title",       i.title);
    take_optional(j, "description", i.description);
    take_optional(j, "websiteUrl",  i.website_url);
    take_optional(j, "icons",       i.icons);
    take_optional_json(j, "_meta",  i.meta);
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
    put_optional(j, "tasks",        c.tasks);
}

void from_json(const nlohmann::json& j, ClientCapabilities& c) {
    take_optional_json(j, "experimental", c.experimental);
    take_optional      (j, "roots",        c.roots);
    take_optional      (j, "sampling",     c.sampling);
    take_optional      (j, "elicitation",  c.elicitation);
    take_optional      (j, "tasks",        c.tasks);
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
    put_optional(j, "tasks",        c.tasks);
}

void from_json(const nlohmann::json& j, ServerCapabilities& c) {
    take_optional_json(j, "experimental", c.experimental);
    take_optional_json(j, "logging",      c.logging);
    take_optional_json(j, "completions",  c.completions);
    take_optional      (j, "prompts",      c.prompts);
    take_optional      (j, "resources",    c.resources);
    take_optional      (j, "tools",        c.tools);
    take_optional      (j, "tasks",        c.tasks);
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
    put_optional(j, "_meta",        r.meta);
}

void from_json(const nlohmann::json& j, InitializeResult& r) {
    r.protocol_version = require<std::string>(j, "protocolVersion");
    r.capabilities     = j.at("capabilities").get<ServerCapabilities>();
    r.server_info      = j.at("serverInfo").get<Implementation>();
    take_optional(j, "instructions", r.instructions);
    take_optional_json(j, "_meta",   r.meta);
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
    put_optional(j, "icons",       r.icons);
    put_optional(j, "_meta",       r.meta);
}

void from_json(const nlohmann::json& j, Resource& r) {
    r.uri  = require<std::string>(j, "uri");
    r.name = require<std::string>(j, "name");
    take_optional(j, "title",       r.title);
    take_optional(j, "description", r.description);
    take_optional(j, "mimeType",    r.mime_type);
    take_optional(j, "annotations", r.annotations);
    take_optional(j, "size",        r.size);
    take_optional(j, "icons",       r.icons);
    take_optional_json(j, "_meta",  r.meta);
}

void to_json(nlohmann::json& j, const ResourceTemplate& r) {
    j = nlohmann::json::object();
    j["uriTemplate"] = r.uri_template;
    j["name"]        = r.name;
    put_optional(j, "title",       r.title);
    put_optional(j, "description", r.description);
    put_optional(j, "mimeType",    r.mime_type);
    put_optional(j, "annotations", r.annotations);
    put_optional(j, "icons",       r.icons);
    put_optional(j, "_meta",       r.meta);
}

void from_json(const nlohmann::json& j, ResourceTemplate& r) {
    r.uri_template = require<std::string>(j, "uriTemplate");
    r.name         = require<std::string>(j, "name");
    take_optional(j, "title",       r.title);
    take_optional(j, "description", r.description);
    take_optional(j, "mimeType",    r.mime_type);
    take_optional(j, "annotations", r.annotations);
    take_optional(j, "icons",       r.icons);
    take_optional_json(j, "_meta",  r.meta);
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
    put_optional(j, "icons",       r.icons);
    put_optional(j, "_meta",       r.meta);
}

void from_json(const nlohmann::json& j, ResourceLink& r) {
    r.uri  = require<std::string>(j, "uri");
    r.name = require<std::string>(j, "name");
    take_optional(j, "title",       r.title);
    take_optional(j, "description", r.description);
    take_optional(j, "mimeType",    r.mime_type);
    take_optional(j, "annotations", r.annotations);
    take_optional(j, "size",        r.size);
    take_optional(j, "icons",       r.icons);
    take_optional_json(j, "_meta",  r.meta);
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

void to_json(nlohmann::json& j, const ToolUseContent& c) {
    j = nlohmann::json::object();
    j["type"]  = "tool_use";
    j["id"]    = c.id;
    j["name"]  = c.name;
    j["input"] = c.input;
    put_optional(j, "annotations", c.annotations);
}
void from_json(const nlohmann::json& j, ToolUseContent& c) {
    c.id    = require<std::string>(j, "id");
    c.name  = require<std::string>(j, "name");
    c.input = require<nlohmann::json>(j, "input");
    take_optional(j, "annotations", c.annotations);
}

// PlainContentBlock is the variant exclusion of tool_use/tool_result.
void to_json(nlohmann::json& j, const PlainContentBlock& c) {
    std::visit([&j](const auto& v) { j = v; }, c);
}
void from_json(const nlohmann::json& j, PlainContentBlock& c) {
    const auto t = require<std::string>(j, "type");
    if (t == "text")          { c = j.get<TextContent>();      return; }
    if (t == "image")         { c = j.get<ImageContent>();     return; }
    if (t == "audio")         { c = j.get<AudioContent>();     return; }
    if (t == "resource_link") { c = j.get<ResourceLink>();     return; }
    if (t == "resource")      { c = j.get<EmbeddedResource>(); return; }
    throw Error(error_code::parse_error,
                std::string{"plain content block must not be \"" + t + "\""});
}

void to_json(nlohmann::json& j, const ToolResultContent& c) {
    j = nlohmann::json::object();
    j["type"]      = "tool_result";
    j["toolUseId"] = c.tool_use_id;
    j["content"]   = c.content;
    put_optional(j, "isError",     c.is_error);
    put_optional(j, "annotations", c.annotations);
}
void from_json(const nlohmann::json& j, ToolResultContent& c) {
    c.tool_use_id = require<std::string>(j, "toolUseId");
    c.content     = j.at("content").get<std::vector<PlainContentBlock>>();
    take_optional(j, "isError",     c.is_error);
    take_optional(j, "annotations", c.annotations);
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
    if (t == "tool_use")      { c = j.get<ToolUseContent>();   return; }
    if (t == "tool_result")   { c = j.get<ToolResultContent>();return; }
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

void to_json(nlohmann::json& j, TaskSupport t) {
    switch (t) {
        case TaskSupport::forbidden: j = "forbidden"; return;
        case TaskSupport::optional:  j = "optional";  return;
        case TaskSupport::required:  j = "required";  return;
    }
    j = "optional";  // unreachable; defensive default
}
void from_json(const nlohmann::json& j, TaskSupport& t) {
    const auto v = j.get<std::string>();
    if      (v == "forbidden") t = TaskSupport::forbidden;
    else if (v == "optional")  t = TaskSupport::optional;
    else if (v == "required")  t = TaskSupport::required;
    else throw Error(error_code::invalid_params,
                     "tool execution: unknown taskSupport \"" + v + "\"");
}

void to_json(nlohmann::json& j, const ToolExecution& e) {
    j = nlohmann::json::object();
    j["taskSupport"] = e.task_support;
}
void from_json(const nlohmann::json& j, ToolExecution& e) {
    if (auto it = j.find("taskSupport"); it != j.end()) {
        e.task_support = it->get<TaskSupport>();
    } else {
        e.task_support = TaskSupport::optional;
    }
}

void to_json(nlohmann::json& j, const Tool& t) {
    j = nlohmann::json::object();
    j["name"]        = t.name;
    j["inputSchema"] = t.input_schema;
    put_optional(j, "title",        t.title);
    put_optional(j, "description",  t.description);
    put_optional(j, "outputSchema", t.output_schema);
    put_optional(j, "annotations",  t.annotations);
    put_optional(j, "icons",        t.icons);
    put_optional(j, "execution",    t.execution);
    put_optional(j, "_meta",        t.meta);
}

void from_json(const nlohmann::json& j, Tool& t) {
    t.name         = require<std::string>(j, "name");
    t.input_schema = require<nlohmann::json>(j, "inputSchema");
    take_optional      (j, "title",        t.title);
    take_optional      (j, "description",  t.description);
    take_optional_json (j, "outputSchema", t.output_schema);
    take_optional      (j, "annotations",  t.annotations);
    take_optional      (j, "icons",        t.icons);
    take_optional      (j, "execution",    t.execution);
    take_optional_json (j, "_meta",        t.meta);
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
    put_optional(j, "_meta",      r.meta);
}

void from_json(const nlohmann::json& j, ListToolsResult& r) {
    r.tools = j.at("tools").get<std::vector<Tool>>();
    take_optional      (j, "nextCursor", r.next_cursor);
    take_optional_json (j, "_meta",      r.meta);
}

void to_json(nlohmann::json& j, const CallToolRequestParams& p) {
    j = nlohmann::json::object();
    j["name"] = p.name;
    put_optional(j, "arguments", p.arguments);
    put_optional(j, "task",      p.task);
}

void from_json(const nlohmann::json& j, CallToolRequestParams& p) {
    p.name = require<std::string>(j, "name");
    take_optional_json(j, "arguments", p.arguments);
    take_optional      (j, "task",     p.task);
}

void to_json(nlohmann::json& j, const CallToolResult& r) {
    j = nlohmann::json::object();
    j["content"] = r.content;
    put_optional(j, "structuredContent", r.structured_content);
    put_optional(j, "isError",           r.is_error);
    put_optional(j, "_meta",             r.meta);
}

void from_json(const nlohmann::json& j, CallToolResult& r) {
    r.content = j.at("content").get<std::vector<ContentBlock>>();
    take_optional_json(j, "structuredContent", r.structured_content);
    take_optional      (j, "isError",          r.is_error);
    take_optional_json (j, "_meta",            r.meta);
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
    put_optional(j, "_meta",      r.meta);
}
void from_json(const nlohmann::json& j, ListResourcesResult& r) {
    r.resources = j.at("resources").get<std::vector<Resource>>();
    take_optional      (j, "nextCursor", r.next_cursor);
    take_optional_json (j, "_meta",      r.meta);
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
    put_optional(j, "_meta",      r.meta);
}
void from_json(const nlohmann::json& j, ListResourceTemplatesResult& r) {
    r.resource_templates = j.at("resourceTemplates").get<std::vector<ResourceTemplate>>();
    take_optional      (j, "nextCursor", r.next_cursor);
    take_optional_json (j, "_meta",      r.meta);
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
    put_optional(j, "_meta", r.meta);
}
void from_json(const nlohmann::json& j, ReadResourceResult& r) {
    r.contents = j.at("contents").get<std::vector<ResourceContents>>();
    take_optional_json(j, "_meta", r.meta);
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
    put_optional(j, "icons",       p.icons);
    put_optional(j, "_meta",       p.meta);
}
void from_json(const nlohmann::json& j, Prompt& p) {
    p.name = require<std::string>(j, "name");
    take_optional(j, "title",       p.title);
    take_optional(j, "description", p.description);
    if (auto it = j.find("arguments"); it != j.end() && !it->is_null()) {
        p.arguments = it->get<std::vector<PromptArgument>>();
    }
    take_optional      (j, "icons", p.icons);
    take_optional_json (j, "_meta", p.meta);
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
    put_optional(j, "_meta",      r.meta);
}
void from_json(const nlohmann::json& j, ListPromptsResult& r) {
    r.prompts = j.at("prompts").get<std::vector<Prompt>>();
    take_optional      (j, "nextCursor", r.next_cursor);
    take_optional_json (j, "_meta",      r.meta);
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
    put_optional(j, "_meta",       r.meta);
}
void from_json(const nlohmann::json& j, GetPromptResult& r) {
    take_optional(j, "description", r.description);
    r.messages = j.at("messages").get<std::vector<PromptMessage>>();
    take_optional_json (j, "_meta", r.meta);
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
    if (j.is_string())              { t = ProgressToken{j.get<std::string>()};   return; }
    if (j.is_number_integer())      { t = ProgressToken{j.get<std::int64_t>()};  return; }
    if (j.is_number_float()) {
        // Same rationale as RequestId — accept whole-number floats.
        const double v = j.get<double>();
        const auto truncated = static_cast<std::int64_t>(v);
        if (static_cast<double>(truncated) != v) {
            throw Error(error_code::parse_error,
                        "progressToken must be an integer (got fractional number)");
        }
        t = ProgressToken{truncated};
        return;
    }
    throw Error(error_code::parse_error,
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

// =====================================================================
// Sampling
// =====================================================================

void to_json(nlohmann::json& j, const ModelHint& h) {
    j = nlohmann::json::object();
    put_optional(j, "name", h.name);
}
void from_json(const nlohmann::json& j, ModelHint& h) {
    take_optional(j, "name", h.name);
}

void to_json(nlohmann::json& j, const ModelPreferences& p) {
    j = nlohmann::json::object();
    if (p.hints.has_value()) j["hints"] = *p.hints;
    put_optional(j, "costPriority",         p.cost_priority);
    put_optional(j, "speedPriority",        p.speed_priority);
    put_optional(j, "intelligencePriority", p.intelligence_priority);
}
void from_json(const nlohmann::json& j, ModelPreferences& p) {
    if (auto it = j.find("hints"); it != j.end() && it->is_array()) {
        p.hints = it->get<std::vector<ModelHint>>();
    }
    take_optional(j, "costPriority",         p.cost_priority);
    take_optional(j, "speedPriority",        p.speed_priority);
    take_optional(j, "intelligencePriority", p.intelligence_priority);
}

std::string_view to_string(IncludeContext c) noexcept {
    switch (c) {
        case IncludeContext::none:        return "none";
        case IncludeContext::this_server: return "thisServer";
        case IncludeContext::all_servers: return "allServers";
    }
    return "?";
}
void to_json(nlohmann::json& j, const IncludeContext& c) { j = std::string{to_string(c)}; }
void from_json(const nlohmann::json& j, IncludeContext& c) {
    const auto s = j.get<std::string>();
    if (s == "none")        c = IncludeContext::none;
    else if (s == "thisServer") c = IncludeContext::this_server;
    else if (s == "allServers") c = IncludeContext::all_servers;
    else throw Error(error_code::parse_error,
                     "unknown includeContext: " + s);
}

std::string_view to_string(ToolChoiceMode m) noexcept {
    switch (m) {
        case ToolChoiceMode::auto_: return "auto";
        case ToolChoiceMode::any:   return "any";
        case ToolChoiceMode::none:  return "none";
        case ToolChoiceMode::tool:  return "tool";
    }
    return "?";
}
void to_json(nlohmann::json& j, const ToolChoiceMode& m) { j = std::string{to_string(m)}; }
void from_json(const nlohmann::json& j, ToolChoiceMode& m) {
    const auto s = j.get<std::string>();
    if      (s == "auto")     m = ToolChoiceMode::auto_;
    else if (s == "any")      m = ToolChoiceMode::any;
    else if (s == "none")     m = ToolChoiceMode::none;
    else if (s == "tool")     m = ToolChoiceMode::tool;
    else throw Error(error_code::parse_error, "unknown toolChoice mode: " + s);
}

// Tagged-by-shape: when `mode == tool` we MUST emit the object form
// `{ type: "tool", name: "..." }`. Other modes serialise as a bare
// string. on the wire. Symmetrically, from_json accepts either form.
void to_json(nlohmann::json& j, const ToolChoice& c) {
    if (c.mode.has_value() && *c.mode == ToolChoiceMode::tool) {
        j = nlohmann::json::object();
        j["type"] = "tool";
        if (c.name.has_value()) j["name"] = *c.name;
        return;
    }
    if (c.mode.has_value()) {
        j = std::string{to_string(*c.mode)};
        return;
    }
    j = nlohmann::json::object();
}
void from_json(const nlohmann::json& j, ToolChoice& c) {
    if (j.is_string()) {
        c.mode = j.get<ToolChoiceMode>();
        return;
    }
    if (j.is_object()) {
        if (auto it = j.find("type"); it != j.end() && it->is_string()) {
            const auto t = it->get<std::string>();
            if (t == "tool") {
                c.mode = ToolChoiceMode::tool;
                take_optional(j, "name", c.name);
                return;
            }
            // Unknown object shape — fall through to the legacy
            // `{ mode: ... }` form for forward compat.
        }
        take_optional(j, "mode", c.mode);
        take_optional(j, "name", c.name);
        return;
    }
    throw Error(error_code::parse_error,
                "toolChoice must be a string or an object");
}

void to_json(nlohmann::json& j, const SamplingMessage& m) {
    j = nlohmann::json::object();
    j["role"] = m.role;
    if (m.content.size() == 1) {
        j["content"] = m.content.front();
    } else {
        j["content"] = m.content;
    }
}
void from_json(const nlohmann::json& j, SamplingMessage& m) {
    m.role = j.at("role").get<Role>();
    const auto& c = j.at("content");
    if (c.is_array()) {
        m.content.clear();
        m.content.reserve(c.size());
        for (const auto& block : c) m.content.push_back(block.get<ContentBlock>());
    } else {
        m.content = { c.get<ContentBlock>() };
    }
}

void to_json(nlohmann::json& j, const CreateMessageRequestParams& p) {
    j = nlohmann::json::object();
    j["messages"]  = p.messages;
    j["maxTokens"] = p.max_tokens;
    put_optional(j, "modelPreferences", p.model_preferences);
    put_optional(j, "systemPrompt",     p.system_prompt);
    put_optional(j, "includeContext",   p.include_context);
    put_optional(j, "temperature",      p.temperature);
    if (p.stop_sequences.has_value()) j["stopSequences"] = *p.stop_sequences;
    put_optional(j, "metadata",         p.metadata);
    put_optional(j, "tools",            p.tools);
    put_optional(j, "toolChoice",       p.tool_choice);
}
void from_json(const nlohmann::json& j, CreateMessageRequestParams& p) {
    p.messages = j.at("messages").get<std::vector<SamplingMessage>>();

    // maxTokens: nlohmann silently clamps numeric overflow when extracting
    // as int64_t, and accepts negative values. Validate strictly.
    const auto& mt = j.at("maxTokens");
    if (!mt.is_number_integer() && !mt.is_number_unsigned()) {
        throw Error(error_code::invalid_params,
                    "maxTokens must be an integer");
    }
    if (mt.is_number_integer() && mt.get<std::int64_t>() < 0) {
        throw Error(error_code::invalid_params,
                    "maxTokens must be non-negative");
    }
    if (mt.is_number_unsigned() &&
        mt.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw Error(error_code::invalid_params,
                    "maxTokens overflows int64");
    }
    p.max_tokens = mt.get<std::int64_t>();

    take_optional(j, "modelPreferences", p.model_preferences);
    take_optional(j, "systemPrompt",     p.system_prompt);
    take_optional(j, "includeContext",   p.include_context);
    take_optional(j, "temperature",      p.temperature);
    if (auto it = j.find("stopSequences"); it != j.end() && it->is_array()) {
        p.stop_sequences = it->get<std::vector<std::string>>();
    }
    take_optional_json(j, "metadata", p.metadata);
    take_optional     (j, "tools",      p.tools);
    take_optional     (j, "toolChoice", p.tool_choice);
}

void to_json(nlohmann::json& j, const CreateMessageResult& r) {
    j = nlohmann::json::object();
    j["role"]  = r.role;
    if (r.content.size() == 1) {
        j["content"] = r.content.front();
    } else {
        j["content"] = r.content;
    }
    j["model"] = r.model;
    put_optional(j, "stopReason", r.stop_reason);
    put_optional(j, "_meta",      r.meta);
}
void from_json(const nlohmann::json& j, CreateMessageResult& r) {
    r.role  = j.at("role").get<Role>();
    const auto& c = j.at("content");
    if (c.is_array()) {
        r.content.clear();
        r.content.reserve(c.size());
        for (const auto& block : c) r.content.push_back(block.get<ContentBlock>());
    } else {
        r.content = { c.get<ContentBlock>() };
    }
    r.model = require<std::string>(j, "model");
    take_optional      (j, "stopReason", r.stop_reason);
    take_optional_json (j, "_meta",      r.meta);
}

// =====================================================================
// Roots
// =====================================================================

void to_json(nlohmann::json& j, const Root& r) {
    j = nlohmann::json{{"uri", r.uri}};
    put_optional(j, "name", r.name);
}
void from_json(const nlohmann::json& j, Root& r) {
    r.uri = require<std::string>(j, "uri");
    take_optional(j, "name", r.name);
}

void to_json(nlohmann::json& j, const ListRootsResult& r) {
    j = nlohmann::json::object();
    j["roots"] = r.roots;
}
void from_json(const nlohmann::json& j, ListRootsResult& r) {
    r.roots = j.at("roots").get<std::vector<Root>>();
}

// =====================================================================
// Completion
// =====================================================================

void to_json(nlohmann::json& j, const ResourceTemplateReference& r) {
    j = nlohmann::json{{"type", "ref/resource"}, {"uri", r.uri_template}};
}
void from_json(const nlohmann::json& j, ResourceTemplateReference& r) {
    r.uri_template = require<std::string>(j, "uri");
}

void to_json(nlohmann::json& j, const PromptReference& r) {
    j = nlohmann::json::object();
    j["type"] = "ref/prompt";
    j["name"] = r.name;
    put_optional(j, "title", r.title);
}
void from_json(const nlohmann::json& j, PromptReference& r) {
    r.name = require<std::string>(j, "name");
    take_optional(j, "title", r.title);
}

void to_json(nlohmann::json& j, const CompletionReference& r) {
    std::visit([&j](const auto& v) { j = v; }, r);
}
void from_json(const nlohmann::json& j, CompletionReference& r) {
    const auto t = require<std::string>(j, "type");
    if (t == "ref/resource") { r = j.get<ResourceTemplateReference>(); return; }
    if (t == "ref/prompt")   { r = j.get<PromptReference>();           return; }
    throw Error(error_code::parse_error,
                std::string{"unknown completion reference type: "} + t);
}

void to_json(nlohmann::json& j, const CompleteArgument& a) {
    j = nlohmann::json{{"name", a.name}, {"value", a.value}};
}
void from_json(const nlohmann::json& j, CompleteArgument& a) {
    a.name  = require<std::string>(j, "name");
    a.value = require<std::string>(j, "value");
}

void to_json(nlohmann::json& j, const CompleteRequestParams& p) {
    j = nlohmann::json::object();
    j["ref"]      = p.reference;
    j["argument"] = p.argument;
    if (p.context_arguments.has_value()) {
        nlohmann::json ctx = nlohmann::json::object();
        for (const auto& [k, v] : *p.context_arguments) ctx[k] = v;
        j["context"] = nlohmann::json{{"arguments", ctx}};
    }
}
void from_json(const nlohmann::json& j, CompleteRequestParams& p) {
    p.reference = j.at("ref").get<CompletionReference>();
    p.argument  = j.at("argument").get<CompleteArgument>();
    if (auto it = j.find("context"); it != j.end() && it->is_object()) {
        if (auto a = it->find("arguments"); a != it->end() && a->is_object()) {
            std::unordered_map<std::string, std::string> ctx;
            for (auto i = a->begin(); i != a->end(); ++i) {
                if (i.value().is_string()) ctx.emplace(i.key(), i.value().get<std::string>());
            }
            p.context_arguments = std::move(ctx);
        }
    }
}

void to_json(nlohmann::json& j, const CompletionValues& v) {
    j = nlohmann::json::object();
    j["values"] = v.values;
    put_optional(j, "total",   v.total);
    put_optional(j, "hasMore", v.has_more);
}
void from_json(const nlohmann::json& j, CompletionValues& v) {
    v.values = j.at("values").get<std::vector<std::string>>();
    take_optional(j, "total",   v.total);
    take_optional(j, "hasMore", v.has_more);
}

void to_json(nlohmann::json& j, const CompleteResult& r) {
    j = nlohmann::json::object();
    j["completion"] = r.completion;
}
void from_json(const nlohmann::json& j, CompleteResult& r) {
    r.completion = j.at("completion").get<CompletionValues>();
}

// =====================================================================
// Elicitation
// =====================================================================

void to_json(nlohmann::json& j, const ElicitFormRequestParams& p) {
    j = nlohmann::json::object();
    // mode is optional and defaults to "form"; we omit it to match the
    // canonical wire form most servers and validators emit.
    j["message"]         = p.message;
    j["requestedSchema"] = p.requested_schema;
}
void from_json(const nlohmann::json& j, ElicitFormRequestParams& p) {
    p.message          = require<std::string>(j, "message");
    p.requested_schema = require<nlohmann::json>(j, "requestedSchema");
    // Per spec, requestedSchema is a JSON-Schema object describing
    // the expected response shape. Reject non-object values up
    // front rather than letting them propagate to a confused
    // handler.
    if (!p.requested_schema.is_object()) {
        throw Error(error_code::invalid_params,
                    "elicitation/create: `requestedSchema` must be a JSON object");
    }
}

void to_json(nlohmann::json& j, const ElicitUrlRequestParams& p) {
    j = nlohmann::json::object();
    j["mode"]          = "url";
    j["message"]       = p.message;
    j["url"]           = p.url;
    j["elicitationId"] = p.elicitation_id;
}
void from_json(const nlohmann::json& j, ElicitUrlRequestParams& p) {
    p.message        = require<std::string>(j, "message");
    p.url            = require<std::string>(j, "url");
    p.elicitation_id = require<std::string>(j, "elicitationId");
}

void to_json(nlohmann::json& j, const ElicitRequestParams& p) {
    std::visit([&](const auto& alt) { j = alt; }, p);
}
void from_json(const nlohmann::json& j, ElicitRequestParams& p) {
    // Tag-by-`mode`: omitted ⇒ form mode (spec default). A present
    // `mode` field MUST be a string; null and other types are
    // rejected so a future spec extension that ships a
    // non-string-mode shape can't silently fall through to form.
    std::string mode = "form";
    if (auto it = j.find("mode"); it != j.end()) {
        if (!it->is_string()) {
            throw Error(error_code::invalid_params,
                        "elicitation/create: `mode` must be a string");
        }
        mode = it->get<std::string>();
    }
    if (mode == "form") {
        p = j.get<ElicitFormRequestParams>();
    } else if (mode == "url") {
        p = j.get<ElicitUrlRequestParams>();
    } else {
        throw Error(error_code::invalid_params,
                    "elicitation/create: unknown mode \"" + mode + "\"");
    }
}

void to_json(nlohmann::json& j, ElicitAction a) {
    switch (a) {
        case ElicitAction::accept:  j = "accept"; return;
        case ElicitAction::decline: j = "decline"; return;
        case ElicitAction::cancel:  j = "cancel"; return;
    }
    j = "cancel";  // unreachable; defensive default
}
void from_json(const nlohmann::json& j, ElicitAction& a) {
    const auto s = j.get<std::string>();
    if      (s == "accept")  a = ElicitAction::accept;
    else if (s == "decline") a = ElicitAction::decline;
    else if (s == "cancel")  a = ElicitAction::cancel;
    else throw Error(error_code::invalid_params,
                     "elicitation result: unknown action \"" + s + "\"");
}

void to_json(nlohmann::json& j, const ElicitResult& r) {
    j = nlohmann::json::object();
    j["action"] = r.action;
    put_optional(j, "content", r.content);
}
void from_json(const nlohmann::json& j, ElicitResult& r) {
    r.action = j.at("action").get<ElicitAction>();
    take_optional_json(j, "content", r.content);
}

void to_json(nlohmann::json& j, const ElicitationCompleteNotificationParams& p) {
    j = nlohmann::json::object();
    j["elicitationId"] = p.elicitation_id;
}
void from_json(const nlohmann::json& j,
               ElicitationCompleteNotificationParams& p) {
    p.elicitation_id = require<std::string>(j, "elicitationId");
}

void to_json(nlohmann::json& j, const UrlElicitationRequiredErrorData& d) {
    j = nlohmann::json::object();
    j["elicitations"] = d.elicitations;
}
void from_json(const nlohmann::json& j, UrlElicitationRequiredErrorData& d) {
    d.elicitations = j.at("elicitations").get<std::vector<ElicitUrlRequestParams>>();
}

// =====================================================================
// Tasks
// =====================================================================

void to_json(nlohmann::json& j, TaskStatus s) {
    switch (s) {
        case TaskStatus::working:        j = "working";        return;
        case TaskStatus::input_required: j = "input_required"; return;
        case TaskStatus::completed:      j = "completed";      return;
        case TaskStatus::failed:         j = "failed";         return;
        case TaskStatus::cancelled:      j = "cancelled";      return;
    }
    j = "failed";  // unreachable; defensive default
}
void from_json(const nlohmann::json& j, TaskStatus& s) {
    const auto v = j.get<std::string>();
    if      (v == "working")        s = TaskStatus::working;
    else if (v == "input_required") s = TaskStatus::input_required;
    else if (v == "completed")      s = TaskStatus::completed;
    else if (v == "failed")         s = TaskStatus::failed;
    else if (v == "cancelled")      s = TaskStatus::cancelled;
    else throw Error(error_code::invalid_params,
                     "task: unknown status \"" + v + "\"");
}

void to_json(nlohmann::json& j, const Task& t) {
    j = nlohmann::json::object();
    j["taskId"]        = t.taskId;
    j["status"]        = t.status;
    j["createdAt"]     = t.created_at;
    j["lastUpdatedAt"] = t.last_updated_at;
    put_optional(j, "statusMessage", t.status_message);
    put_optional(j, "ttl",           t.ttl);
    put_optional(j, "pollInterval",  t.poll_interval);
}
void from_json(const nlohmann::json& j, Task& t) {
    t.taskId          = require<std::string>(j, "taskId");
    t.status          = j.at("status").get<TaskStatus>();
    t.created_at      = require<std::string>(j, "createdAt");
    t.last_updated_at = require<std::string>(j, "lastUpdatedAt");
    take_optional(j, "statusMessage", t.status_message);
    take_optional(j, "ttl",           t.ttl);
    take_optional(j, "pollInterval",  t.poll_interval);
}

void to_json(nlohmann::json& j, const TaskAugmentation& t) {
    j = nlohmann::json::object();
    put_optional(j, "ttl", t.ttl);
}
void from_json(const nlohmann::json& j, TaskAugmentation& t) {
    take_optional(j, "ttl", t.ttl);
}

void to_json(nlohmann::json& j, const CreateTaskResult& r) {
    j = nlohmann::json::object();
    j["task"] = r.task;
}
void from_json(const nlohmann::json& j, CreateTaskResult& r) {
    r.task = j.at("task").get<Task>();
}

void to_json(nlohmann::json& j, const GetTaskRequestParams& p) {
    j = nlohmann::json::object();
    j["taskId"] = p.task_id;
}
void from_json(const nlohmann::json& j, GetTaskRequestParams& p) {
    p.task_id = require<std::string>(j, "taskId");
}

void to_json(nlohmann::json& j, const GetTaskResultRequestParams& p) {
    j = nlohmann::json::object();
    j["taskId"] = p.task_id;
}
void from_json(const nlohmann::json& j, GetTaskResultRequestParams& p) {
    p.task_id = require<std::string>(j, "taskId");
}

void to_json(nlohmann::json& j, const ListTasksRequestParams& p) {
    j = nlohmann::json::object();
    put_optional(j, "cursor", p.cursor);
}
void from_json(const nlohmann::json& j, ListTasksRequestParams& p) {
    take_optional(j, "cursor", p.cursor);
}

void to_json(nlohmann::json& j, const ListTasksResult& r) {
    j = nlohmann::json::object();
    j["tasks"] = r.tasks;
    put_optional(j, "nextCursor", r.next_cursor);
}
void from_json(const nlohmann::json& j, ListTasksResult& r) {
    r.tasks = j.at("tasks").get<std::vector<Task>>();
    take_optional(j, "nextCursor", r.next_cursor);
}

void to_json(nlohmann::json& j, const CancelTaskRequestParams& p) {
    j = nlohmann::json::object();
    j["taskId"] = p.task_id;
}
void from_json(const nlohmann::json& j, CancelTaskRequestParams& p) {
    p.task_id = require<std::string>(j, "taskId");
}

void to_json(nlohmann::json& j, const TaskStatusNotificationParams& p) {
    // Notifications carry the same Task projection inline (no
    // wrapping object), per spec.
    j = p.task;
}
void from_json(const nlohmann::json& j, TaskStatusNotificationParams& p) {
    p.task = j.get<Task>();
}

void to_json(nlohmann::json& j, const TasksRequestsCapability& c) {
    j = nlohmann::json::object();
    put_optional(j, "tools",       c.tools);
    put_optional(j, "sampling",    c.sampling);
    put_optional(j, "elicitation", c.elicitation);
}
void from_json(const nlohmann::json& j, TasksRequestsCapability& c) {
    take_optional_json(j, "tools",       c.tools);
    take_optional_json(j, "sampling",    c.sampling);
    take_optional_json(j, "elicitation", c.elicitation);
}

void to_json(nlohmann::json& j, const TasksCapability& c) {
    j = nlohmann::json::object();
    put_optional(j, "list",     c.list);
    put_optional(j, "cancel",   c.cancel);
    put_optional(j, "requests", c.requests);
}
void from_json(const nlohmann::json& j, TasksCapability& c) {
    take_optional_json(j, "list",     c.list);
    take_optional_json(j, "cancel",   c.cancel);
    take_optional      (j, "requests", c.requests);
}

}  // namespace mcp
