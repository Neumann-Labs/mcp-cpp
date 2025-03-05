#ifndef MCP_TYPES_HPP_
#define MCP_TYPES_HPP_

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mcp {
namespace types {

/**
 * @brief Standard JSON-RPC 2.0 error codes and MCP-specific error codes
 */
enum class ErrorCode {
  // JSON-RPC 2.0 standard error codes
  ParseError = -32700,     ///< Invalid JSON was received
  InvalidRequest = -32600, ///< The JSON sent is not a valid Request object
  MethodNotFound = -32601, ///< The method does not exist / is not available
  InvalidParams = -32602,  ///< Invalid method parameter(s)
  InternalError = -32603,  ///< Internal JSON-RPC error

  // MCP-specific error codes
  ProtocolError = -32000,     ///< Protocol-related error
  TransportError = -32001,    ///< Transport-related error
  TimeoutError = -32002,      ///< Operation timed out
  AuthorizationError = -32003 ///< Authorization failed
};

/**
 * @brief Structure representing an error in JSON-RPC 2.0
 */
struct ErrorData {
  int code;            ///< Error code
  std::string message; ///< Error message
  nlohmann::json data; ///< Optional additional error data
};

/**
 * @brief Common metadata for request parameters
 */
struct RequestMeta {
  std::optional<std::string> progressToken; ///< Token for tracking progress
};

/**
 * @brief Base structure for request parameters
 */
struct RequestParams {
  std::optional<RequestMeta> meta; ///< Optional metadata
};

/**
 * @brief Common metadata for notification parameters
 */
struct NotificationMeta {
  // Additional metadata for notifications
};

/**
 * @brief Base structure for notification parameters
 */
struct NotificationParams {
  std::optional<NotificationMeta> meta; ///< Optional metadata
};

/**
 * @brief Base structure for result data
 */
struct Result {
  std::optional<nlohmann::json> meta; ///< Optional metadata
};

/**
 * @brief JSON-RPC 2.0 request message
 */
struct JSONRPCRequest {
  std::string jsonrpc = "2.0";          ///< JSON-RPC version (always "2.0")
  std::variant<std::string, int> id;    ///< Request identifier
  std::string method;                   ///< Method name
  std::optional<nlohmann::json> params; ///< Method parameters
};

/**
 * @brief JSON-RPC 2.0 notification message (request without id)
 */
struct JSONRPCNotification {
  std::string jsonrpc = "2.0";          ///< JSON-RPC version (always "2.0")
  std::string method;                   ///< Method name
  std::optional<nlohmann::json> params; ///< Method parameters
};

/**
 * @brief JSON-RPC 2.0 success response message
 */
struct JSONRPCResponse {
  std::string jsonrpc = "2.0";       ///< JSON-RPC version (always "2.0")
  std::variant<std::string, int> id; ///< Request identifier
  nlohmann::json result;             ///< Result data
};

/**
 * @brief JSON-RPC 2.0 error response message
 */
struct JSONRPCError {
  std::string jsonrpc = "2.0";       ///< JSON-RPC version (always "2.0")
  std::variant<std::string, int> id; ///< Request identifier
  ErrorData error;                   ///< Error data
};

/**
 * @brief Variant type that can hold any JSON-RPC 2.0 message
 */
using JSONRPCMessage = std::variant<JSONRPCRequest, JSONRPCNotification,
                                    JSONRPCResponse, JSONRPCError>;

/**
 * @brief Tool parameter schema description
 */
struct ToolParameter {
  std::string name;        ///< Parameter name
  std::string description; ///< Parameter description
  nlohmann::json schema;   ///< JSON Schema for the parameter
  bool required;           ///< Whether the parameter is required
};

/**
 * @brief Tool definition
 */
struct Tool {
  std::string name;                      ///< Tool name
  std::string description;               ///< Tool description
  std::vector<ToolParameter> parameters; ///< Tool parameters
  nlohmann::json returns;                ///< JSON Schema for return value
};

/**
 * @brief Resource definition
 */
struct Resource {
  std::string uri;         ///< Resource URI
  std::string name;        ///< Resource name
  std::string description; ///< Resource description
  std::string contentType; ///< Resource content type (MIME type)
};

/**
 * @brief Prompt argument definition
 */
struct PromptArgument {
  std::string name;        ///< Argument name
  std::string description; ///< Argument description
  nlohmann::json schema;   ///< JSON Schema for the argument
  bool required;           ///< Whether the argument is required
};

/**
 * @brief Prompt definition
 */
struct Prompt {
  std::string name;                      ///< Prompt name
  std::string description;               ///< Prompt description
  std::vector<PromptArgument> arguments; ///< Prompt arguments
};

/**
 * @brief Server capabilities
 */
struct ServerCapabilities {
  bool supportsTools = false;     ///< Server supports tool calls
  bool supportsResources = false; ///< Server supports resources
  bool supportsPrompts = false;   ///< Server supports prompts
};

/**
 * @brief Client capabilities
 */
struct ClientCapabilities {
  bool supportsProgress = false;     ///< Client supports progress reporting
  bool supportsCancellation = false; ///< Client supports cancellation
};

/**
 * @brief Initialize request parameters
 */
struct InitializeParams : RequestParams {
  std::string clientName;          ///< Client name
  std::string clientVersion;       ///< Client version
  ClientCapabilities capabilities; ///< Client capabilities
};

/**
 * @brief Initialize result
 */
struct InitializeResult : Result {
  std::string serverName;                  ///< Server name
  std::string serverVersion;               ///< Server version
  std::optional<std::string> instructions; ///< Optional usage instructions
  ServerCapabilities capabilities;         ///< Server capabilities
};

/**
 * @brief Parameters for listing tools
 */
struct ListToolsParams : RequestParams {
  // Empty for now, may add filtering options later
};

/**
 * @brief Result for listing tools
 */
struct ListToolsResult : Result {
  std::vector<Tool> tools; ///< Available tools
};

/**
 * @brief Parameters for calling a tool
 */
struct CallToolParams : RequestParams {
  std::string name;          ///< Tool name
  nlohmann::json parameters; ///< Tool parameters
};

/**
 * @brief Result from a tool call
 */
struct CallToolResult : Result {
  nlohmann::json result; ///< Tool result
};

/**
 * @brief Parameters for progress notification
 */
struct ProgressParams : NotificationParams {
  std::string token;                  ///< Progress token
  double progress;                    ///< Progress value (0.0 - 1.0)
  std::optional<std::string> message; ///< Optional progress message
};

/**
 * @brief Parameters for listing resources
 */
struct ListResourcesParams : RequestParams {
  // Empty for now, may add filtering options later
};

/**
 * @brief Result for listing resources
 */
struct ListResourcesResult : Result {
  std::vector<Resource> resources; ///< Available resources
};

/**
 * @brief Parameters for reading a resource
 */
struct ReadResourceParams : RequestParams {
  std::string uri; ///< Resource URI
};

/**
 * @brief Result from reading a resource
 */
struct ReadResourceResult : Result {
  std::string contentType; ///< Resource content type (MIME type)
  std::string content; ///< Resource content (may be encoded for binary data)
};

/**
 * @brief Parameters for listing prompts
 */
struct ListPromptsParams : RequestParams {
  // Empty for now, may add filtering options later
};

/**
 * @brief Result for listing prompts
 */
struct ListPromptsResult : Result {
  std::vector<Prompt> prompts; ///< Available prompts
};

/**
 * @brief Parameters for getting a prompt
 */
struct GetPromptParams : RequestParams {
  std::string name;         ///< Prompt name
  nlohmann::json arguments; ///< Prompt arguments
};

/**
 * @brief Result from getting a prompt
 */
struct GetPromptResult : Result {
  std::string content; ///< Prompt content
};

/**
 * @brief Parameters for cancellation notification
 */
struct CancelParams : NotificationParams {
  std::variant<std::string, int> id; ///< ID of the request to cancel
};

/**
 * @brief Parameters for shutdown request
 */
struct ShutdownParams : RequestParams {
  // Empty for now
};

/**
 * @brief Result from shutdown request
 */
struct ShutdownResult : Result {
  // Empty for now
};

} // namespace types
} // namespace mcp

// JSON serialization/deserialization functions
namespace nlohmann {

// ErrorCode enum serialization
template <> struct adl_serializer<mcp::types::ErrorCode> {
  static void to_json(json &j, const mcp::types::ErrorCode &code) {
    j = static_cast<int>(code);
  }

  static void from_json(const json &j, mcp::types::ErrorCode &code) {
    code = static_cast<mcp::types::ErrorCode>(j.get<int>());
  }
};

// ErrorData serialization
template <> struct adl_serializer<mcp::types::ErrorData> {
  static void to_json(json &j, const mcp::types::ErrorData &error) {
    j = json{{"code", error.code}, {"message", error.message}};
    if (!error.data.is_null()) {
      j["data"] = error.data;
    }
  }

  static void from_json(const json &j, mcp::types::ErrorData &error) {
    j.at("code").get_to(error.code);
    j.at("message").get_to(error.message);
    if (j.contains("data")) {
      error.data = j["data"];
    } else {
      error.data = nullptr;
    }
  }
};

// RequestMeta serialization
template <> struct adl_serializer<mcp::types::RequestMeta> {
  static void to_json(json &j, const mcp::types::RequestMeta &meta) {
    j = json::object();
    if (meta.progressToken) {
      j["progressToken"] = *meta.progressToken;
    }
  }

  static void from_json(const json &j, mcp::types::RequestMeta &meta) {
    if (j.contains("progressToken")) {
      meta.progressToken = j["progressToken"].get<std::string>();
    }
  }
};

// JSONRPCRequest serialization
template <> struct adl_serializer<mcp::types::JSONRPCRequest> {
  static void to_json(json &j, const mcp::types::JSONRPCRequest &request) {
    j = json{{"jsonrpc", request.jsonrpc},
             {"id", request.id},
             {"method", request.method}};
    if (request.params) {
      j["params"] = *request.params;
    }
  }

  static void from_json(const json &j, mcp::types::JSONRPCRequest &request) {
    j.at("jsonrpc").get_to(request.jsonrpc);
    j.at("id").get_to(request.id);
    j.at("method").get_to(request.method);
    if (j.contains("params")) {
      request.params = j["params"];
    }
  }
};

// Add other serializers here...

} // namespace nlohmann

#endif // MCP_TYPES_HPP_