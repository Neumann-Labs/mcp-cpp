#ifndef MCP_TRANSPORT_STDIO_TRANSPORT_HPP_
#define MCP_TRANSPORT_STDIO_TRANSPORT_HPP_

#include "mcp/transport/transport.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

namespace mcp {
namespace transport {

/**
 * @brief Transport implementation using standard input/output
 *
 * This class implements the Transport interface using standard input/output.
 * It reads JSON-RPC messages from stdin and writes them to stdout.
 */
class StdioTransport : public Transport {
public:
  /**
   * @brief Constructor
   */
  StdioTransport();

  /**
   * @brief Destructor
   */
  ~StdioTransport() override;

  // Transport interface implementation
  void send(const types::JSONRPCMessage &message,
            std::function<void(const std::error_code &)> callback) override;

  std::error_code send(const types::JSONRPCMessage &message,
                       std::chrono::milliseconds timeout) override;

  void setMessageCallback(
      std::function<void(types::JSONRPCMessage)> callback) override;

  void setErrorCallback(std::function<void(std::error_code)> callback) override;

  void setCloseCallback(std::function<void()> callback) override;

  void connect() override;
  void disconnect() override;
  bool isConnected() const override;

private:
  // Internal types
  struct SendOperation {
    types::JSONRPCMessage message;
    std::function<void(const std::error_code &)> callback;
  };

  // IO handling
  void readLoop();
  void writeLoop();
  void processLine(const std::string &line);

  // Thread management
  std::thread read_thread_;
  std::thread write_thread_;
  std::atomic<bool> running_;

  // Callbacks
  std::function<void(types::JSONRPCMessage)> message_callback_;
  std::function<void(std::error_code)> error_callback_;
  std::function<void()> close_callback_;

  // Thread synchronization
  std::mutex callback_mutex_;

  // Send queue
  std::queue<SendOperation> send_queue_;
  std::mutex send_queue_mutex_;
  std::condition_variable send_queue_cv_;
};

} // namespace transport
} // namespace mcp

#endif // MCP_TRANSPORT_STDIO_TRANSPORT_HPP_