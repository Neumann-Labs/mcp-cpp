#ifndef MCP_UTILS_JSON_UTILS_HPP_
#define MCP_UTILS_JSON_UTILS_HPP_

#include "mcp/types.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace mcp {
namespace json_utils {

/**
 * @brief Validate JSON against a schema
 *
 * @param json The JSON value to validate
 * @param schema The JSON schema to validate against
 * @param error_msg Optional output parameter for error message
 * @return true if validation succeeded, false otherwise
 */
bool validate(const nlohmann::json &json, const nlohmann::json &schema,
              std::string *error_msg = nullptr);

/**
 * @brief Parse a JSON string
 *
 * @param json_str The JSON string to parse
 * @return nlohmann::json The parsed JSON
 * @throws TransportException if parsing fails
 */
nlohmann::json parse(const std::string &json_str);

/**
 * @brief Parse a JSON-RPC message from a string
 *
 * @param json_str The JSON string to parse
 * @return types::JSONRPCMessage The parsed message
 * @throws TransportException if parsing fails
 * @throws ProtocolException if the message is not a valid JSON-RPC message
 */
types::JSONRPCMessage parseMessage(const std::string &json_str);

/**
 * @brief Determine the type of a JSON-RPC message
 *
 * @param json The JSON message
 * @return The message type (request, notification, response, or error)
 * @throws ProtocolException if the message is not a valid JSON-RPC message
 */
enum class MessageType { Request, Notification, Response, Error };

/**
 * @brief Get the type of a JSON-RPC message
 *
 * @param json The JSON message
 * @return MessageType The message type
 * @throws ProtocolException if the message is not a valid JSON-RPC message
 */
MessageType getMessageType(const nlohmann::json &json);

/**
 * @brief Serialize a JSON-RPC message to a string
 *
 * @param message The message to serialize
 * @return std::string The serialized message
 */
std::string serializeMessage(const types::JSONRPCMessage &message);

} // namespace json_utils

// Alias for backward compatibility
namespace utils {
inline types::JSONRPCMessage parse_json_message(const std::string &json_str) {
  return json_utils::parseMessage(json_str);
}

inline std::string serialize_json(const types::JSONRPCMessage &message) {
  return json_utils::serializeMessage(message);
}
} // namespace utils

} // namespace mcp

#endif // MCP_UTILS_JSON_UTILS_HPP_