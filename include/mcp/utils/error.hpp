#ifndef MCP_UTILS_ERROR_HPP_
#define MCP_UTILS_ERROR_HPP_

#include "mcp/types.hpp"
#include <stdexcept>
#include <string>

namespace mcp {

/**
 * @brief Base exception class for MCP-related errors
 *
 * This class extends std::runtime_error and adds specific MCP error data
 */
class MCPException : public std::runtime_error {
public:
  /**
   * @brief Construct a new MCPException with error data
   *
   * @param error The error data
   */
  explicit MCPException(types::ErrorData error);

  /**
   * @brief Construct a new MCPException with error code and message
   *
   * @param code The error code
   * @param message The error message
   */
  explicit MCPException(types::ErrorCode code, const std::string &message);

  /**
   * @brief Get the error data
   *
   * @return const types::ErrorData& The error data
   */
  const types::ErrorData &error() const;

protected:
  /**
   * @brief Get mutable reference to error data for derived classes
   *
   * @return types::ErrorData& The error data
   */
  types::ErrorData &error_data();

private:
  types::ErrorData error_; ///< The error data
};

/**
 * @brief Exception for transport-related errors
 */
class TransportException : public MCPException {
public:
  /**
   * @brief Construct a new TransportException
   *
   * @param message The error message
   * @param data Optional additional data
   */
  explicit TransportException(const std::string &message,
                              const nlohmann::json &data = nullptr);

  /**
   * @brief Construct a new TransportException with error code
   *
   * @param code The error code
   * @param message The error message
   * @param data Optional additional data
   */
  explicit TransportException(types::ErrorCode code, const std::string &message,
                              const nlohmann::json &data = nullptr);

  /**
   * @brief Construct a new TransportException from a std::error_code
   *
   * @param error The std::error_code
   */
  explicit TransportException(const std::error_code &error);
};

/**
 * @brief Exception for protocol-related errors
 */
class ProtocolException : public MCPException {
public:
  /**
   * @brief Construct a new ProtocolException
   *
   * @param message The error message
   * @param data Optional additional data
   */
  explicit ProtocolException(const std::string &message,
                             const nlohmann::json &data = nullptr);

  /**
   * @brief Construct a new ProtocolException with error code
   *
   * @param code The error code
   * @param message The error message
   * @param data Optional additional data
   */
  explicit ProtocolException(types::ErrorCode code, const std::string &message,
                             const nlohmann::json &data = nullptr);

  /**
   * @brief Construct a new ProtocolException from error data
   *
   * @param error The error data
   */
  explicit ProtocolException(const types::ErrorData &error);
};

/**
 * @brief Exception for timeout errors
 */
class TimeoutException : public MCPException {
public:
  /**
   * @brief Construct a new TimeoutException
   *
   * @param message The error message
   * @param data Optional additional data
   */
  explicit TimeoutException(const std::string &message,
                            const nlohmann::json &data = nullptr);
};

/**
 * @brief Create an error response from an exception
 *
 * @param id The request ID
 * @param exception The exception
 * @return types::JSONRPCError The error response
 */
types::JSONRPCError
createErrorResponse(const std::variant<std::string, int> &id,
                    const MCPException &exception);

/**
 * @brief Create an error response from error data
 *
 * @param id The request ID
 * @param error The error data
 * @return types::JSONRPCError The error response
 */
types::JSONRPCError
createErrorResponse(const std::variant<std::string, int> &id,
                    const types::ErrorData &error);

/**
 * @brief Create an error response from error code and message
 *
 * @param id The request ID
 * @param code The error code
 * @param message The error message
 * @param data Optional additional data
 * @return types::JSONRPCError The error response
 */
types::JSONRPCError
createErrorResponse(const std::variant<std::string, int> &id,
                    types::ErrorCode code, const std::string &message,
                    const nlohmann::json &data = nullptr);

// Alias for backward compatibility
namespace utils {
using ProtocolException = mcp::ProtocolException;
using TransportException = mcp::TransportException;
using TimeoutException = mcp::TimeoutException;
} // namespace utils

} // namespace mcp

#endif // MCP_UTILS_ERROR_HPP_