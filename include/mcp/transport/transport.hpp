#ifndef MCP_TRANSPORT_TRANSPORT_HPP_
#define MCP_TRANSPORT_TRANSPORT_HPP_

#include "mcp/types.hpp"
#include <chrono>
#include <functional>
#include <string>
#include <system_error>

namespace mcp {
namespace transport {

/**
 * @brief Abstract base class for transport implementations
 *
 * This class defines the interface for all transport implementations.
 * It provides methods for sending and receiving messages, as well as
 * managing the connection lifecycle.
 */
class Transport {
public:
  /**
   * @brief Virtual destructor
   */
  virtual ~Transport() = default;

  /**
   * @brief Asynchronously send a message
   *
   * @param message The message to send
   * @param callback The callback to invoke when the operation completes
   */
  virtual void send(const types::JSONRPCMessage &message,
                    std::function<void(const std::error_code &)> callback) = 0;

  /**
   * @brief Synchronously send a message with timeout
   *
   * @param message The message to send
   * @param timeout The timeout for the operation
   * @return std::error_code An error code if the operation failed
   */
  virtual std::error_code
  send(const types::JSONRPCMessage &message,
       std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) = 0;

  /**
   * @brief Set the callback for received messages
   *
   * @param callback The callback to invoke when a message is received
   */
  virtual void
  setMessageCallback(std::function<void(types::JSONRPCMessage)> callback) = 0;

  /**
   * @brief Set the callback for transport errors
   *
   * @param callback The callback to invoke when an error occurs
   */
  virtual void
  setErrorCallback(std::function<void(std::error_code)> callback) = 0;

  /**
   * @brief Set the callback for connection closure
   *
   * @param callback The callback to invoke when the connection is closed
   */
  virtual void setCloseCallback(std::function<void()> callback) = 0;

  /**
   * @brief Connect to the transport
   */
  virtual void connect() = 0;

  /**
   * @brief Disconnect from the transport
   */
  virtual void disconnect() = 0;

  /**
   * @brief Check if the transport is connected
   *
   * @return true if connected, false otherwise
   */
  virtual bool isConnected() const = 0;
};

} // namespace transport
} // namespace mcp

#endif // MCP_TRANSPORT_TRANSPORT_HPP_