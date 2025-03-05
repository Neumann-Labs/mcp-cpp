#include "mcp/utils/error.hpp"

namespace mcp {

MCPException::MCPException(types::ErrorData error)
    : std::runtime_error(error.message), error_(std::move(error)) {}

MCPException::MCPException(types::ErrorCode code, const std::string &message)
    : std::runtime_error(message),
      error_({static_cast<int>(code), message, nullptr}) {}

const types::ErrorData &MCPException::error() const { return error_; }

types::ErrorData &MCPException::error_data() { return error_; }

TransportException::TransportException(const std::string &message,
                                       const nlohmann::json &data)
    : MCPException(types::ErrorCode::TransportError, message) {
  if (!data.is_null()) {
    error_data().data = data;
  }
}

TransportException::TransportException(types::ErrorCode code,
                                       const std::string &message,
                                       const nlohmann::json &data)
    : MCPException(code, message) {
  if (!data.is_null()) {
    error_data().data = data;
  }
}

ProtocolException::ProtocolException(const std::string &message,
                                     const nlohmann::json &data)
    : MCPException(types::ErrorCode::ProtocolError, message) {
  if (!data.is_null()) {
    error_data().data = data;
  }
}

ProtocolException::ProtocolException(types::ErrorCode code,
                                     const std::string &message,
                                     const nlohmann::json &data)
    : MCPException(code, message) {
  if (!data.is_null()) {
    error_data().data = data;
  }
}

TimeoutException::TimeoutException(const std::string &message,
                                   const nlohmann::json &data)
    : MCPException(types::ErrorCode::TimeoutError, message) {
  if (!data.is_null()) {
    error_data().data = data;
  }
}

types::JSONRPCError
createErrorResponse(const std::variant<std::string, int> &id,
                    const MCPException &exception) {
  return {.jsonrpc = "2.0", .id = id, .error = exception.error()};
}

types::JSONRPCError
createErrorResponse(const std::variant<std::string, int> &id,
                    const types::ErrorData &error) {
  return {.jsonrpc = "2.0", .id = id, .error = error};
}

types::JSONRPCError
createErrorResponse(const std::variant<std::string, int> &id,
                    types::ErrorCode code, const std::string &message,
                    const nlohmann::json &data) {
  return {.jsonrpc = "2.0",
          .id = id,
          .error = {.code = static_cast<int>(code),
                    .message = message,
                    .data = data}};
}

} // namespace mcp