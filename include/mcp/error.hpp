// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace mcp {

/// MCP / JSON-RPC 2.0 error codes.
///
/// Values in [-32768, -32000] are reserved by JSON-RPC 2.0 for transport-level
/// errors. The MCP spec inherits the standard codes and adds its own in
/// [-32000, -32099]. Server- or domain-specific errors should use
/// application-defined codes outside the reserved range.
namespace error_code {

// JSON-RPC 2.0 standard codes.
inline constexpr int parse_error      = -32700;
inline constexpr int invalid_request  = -32600;
inline constexpr int method_not_found = -32601;
inline constexpr int invalid_params   = -32602;
inline constexpr int internal_error   = -32603;

// MCP-specific (in the implementation-defined JSON-RPC range).
inline constexpr int url_elicitation_required = -32042;

}  // namespace error_code

/// A wire-format-compatible JSON-RPC error object.
///
/// This is the value that lives inside `JSONRPCErrorResponse::error`. It is
/// also raised as `Error` (an exception) when a method call receives an error
/// response and the caller asks for the result via a future.
struct ErrorObject {
    int code{};
    std::string message;
    nlohmann::json data;  // optional; null when absent on the wire

    [[nodiscard]] bool has_data() const noexcept { return !data.is_null(); }
};

/// Exception type for MCP errors. Carries the wire `ErrorObject`.
///
/// Thrown only at well-defined boundaries (e.g. `std::future<T>::get()` for a
/// failed call). Library internals prefer to plumb errors through return
/// values or callbacks rather than throwing.
class Error : public std::runtime_error {
public:
    explicit Error(ErrorObject err)
        : std::runtime_error(err.message), error_(std::move(err)) {}

    Error(int code, std::string msg, nlohmann::json data = {})
        : std::runtime_error(msg),
          error_{code, std::move(msg), std::move(data)} {}

    [[nodiscard]] const ErrorObject& object() const noexcept { return error_; }
    [[nodiscard]] int                code()   const noexcept { return error_.code; }
    [[nodiscard]] const std::string& message() const noexcept { return error_.message; }
    [[nodiscard]] const nlohmann::json& data() const noexcept { return error_.data; }

private:
    ErrorObject error_;
};

}  // namespace mcp
