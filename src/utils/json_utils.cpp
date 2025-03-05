#include "mcp/utils/json_utils.hpp"
#include "mcp/utils/error.hpp"
#include <nlohmann/json-schema.hpp>

namespace mcp {
namespace json_utils {

bool validate(const nlohmann::json &json, const nlohmann::json &schema,
              std::string *error_msg) {
  try {
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(schema);
    validator.validate(json);
    return true;
  } catch (const std::exception &e) {
    if (error_msg) {
      *error_msg = e.what();
    }
    return false;
  }
}

nlohmann::json parse(const std::string &json_str) {
  try {
    return nlohmann::json::parse(json_str);
  } catch (const nlohmann::json::parse_error &e) {
    throw TransportException("JSON parse error: " + std::string(e.what()));
  }
}

MessageType getMessageType(const nlohmann::json &json) {
  // Validate it's a JSON-RPC 2.0 message
  if (!json.is_object() || !json.contains("jsonrpc") ||
      json["jsonrpc"] != "2.0") {
    throw ProtocolException(
        "Invalid JSON-RPC message: missing or invalid jsonrpc version");
  }

  // Check if it's an error response
  if (json.contains("error") && json.contains("id")) {
    return MessageType::Error;
  }

  // Check if it's a successful response
  if (json.contains("result") && json.contains("id")) {
    return MessageType::Response;
  }

  // Check if it's a request
  if (json.contains("method")) {
    if (json.contains("id")) {
      return MessageType::Request;
    } else {
      return MessageType::Notification;
    }
  }

  throw ProtocolException(
      "Invalid JSON-RPC message: cannot determine message type");
}

types::JSONRPCMessage parseMessage(const std::string &json_str) {
  nlohmann::json json = parse(json_str);

  try {
    MessageType type = getMessageType(json);

    switch (type) {
    case MessageType::Request:
      return json.get<types::JSONRPCRequest>();

    case MessageType::Notification:
      return json.get<types::JSONRPCNotification>();

    case MessageType::Response:
      return json.get<types::JSONRPCResponse>();

    case MessageType::Error:
      return json.get<types::JSONRPCError>();

    default:
      throw ProtocolException("Unknown message type");
    }
  } catch (const nlohmann::json::exception &e) {
    throw ProtocolException("JSON-RPC message parse error: " +
                            std::string(e.what()));
  }
}

std::string serializeMessage(const types::JSONRPCMessage &message) {
  return std::visit([](const auto &msg) { return nlohmann::json(msg).dump(); },
                    message);
}

} // namespace json_utils
} // namespace mcp