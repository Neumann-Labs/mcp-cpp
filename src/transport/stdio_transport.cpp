#include "mcp/transport/stdio_transport.hpp"
#include "mcp/utils/error.hpp"
#include "mcp/utils/json_utils.hpp"
#include "mcp/utils/logging.hpp"
#include <future>
#include <iostream>
#include <system_error>

namespace mcp {
namespace transport {

namespace {
// Error categories
enum class StdioError { Timeout, Disconnected, WriteError, ReadError };

class StdioErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "stdio"; }

  std::string message(int ev) const override {
    switch (static_cast<StdioError>(ev)) {
    case StdioError::Timeout:
      return "Operation timed out";
    case StdioError::Disconnected:
      return "Transport disconnected";
    case StdioError::WriteError:
      return "Write error";
    case StdioError::ReadError:
      return "Read error";
    default:
      return "Unknown error";
    }
  }
};

const std::error_category &stdio_category() {
  static StdioErrorCategory category;
  return category;
}

std::error_code make_error_code(StdioError e) {
  return {static_cast<int>(e), stdio_category()};
}
} // namespace

StdioTransport::StdioTransport() : running_(false) {}

StdioTransport::~StdioTransport() { disconnect(); }

void StdioTransport::send(
    const types::JSONRPCMessage &message,
    std::function<void(const std::error_code &)> callback) {
  if (!running_) {
    if (callback) {
      callback(make_error_code(StdioError::Disconnected));
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(send_queue_mutex_);
    send_queue_.push({message, callback});
  }

  send_queue_cv_.notify_one();
}

std::error_code StdioTransport::send(const types::JSONRPCMessage &message,
                                     std::chrono::milliseconds timeout) {
  if (!running_) {
    return make_error_code(StdioError::Disconnected);
  }

  std::promise<std::error_code> promise;
  auto future = promise.get_future();

  send(message,
       [&promise](const std::error_code &ec) { promise.set_value(ec); });

  if (future.wait_for(timeout) == std::future_status::timeout) {
    return make_error_code(StdioError::Timeout);
  }

  return future.get();
}

void StdioTransport::setMessageCallback(
    std::function<void(types::JSONRPCMessage)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  message_callback_ = callback;
}

void StdioTransport::setErrorCallback(
    std::function<void(std::error_code)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  error_callback_ = callback;
}

void StdioTransport::setCloseCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  close_callback_ = callback;
}

void StdioTransport::connect() {
  if (running_) {
    return;
  }

  running_ = true;

  // Start threads
  read_thread_ = std::thread(&StdioTransport::readLoop, this);
  write_thread_ = std::thread(&StdioTransport::writeLoop, this);

  MCP_LOG_INFO("StdioTransport connected");
}

void StdioTransport::disconnect() {
  if (!running_) {
    return;
  }

  running_ = false;

  // Notify write thread to exit
  send_queue_cv_.notify_all();

  // Wait for threads to exit
  if (read_thread_.joinable()) {
    read_thread_.join();
  }

  if (write_thread_.joinable()) {
    write_thread_.join();
  }

  // Clear queue
  {
    std::lock_guard<std::mutex> lock(send_queue_mutex_);
    while (!send_queue_.empty()) {
      auto &op = send_queue_.front();
      if (op.callback) {
        op.callback(make_error_code(StdioError::Disconnected));
      }
      send_queue_.pop();
    }
  }

  // Notify close
  std::function<void()> close_callback;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    close_callback = close_callback_;
  }

  if (close_callback) {
    try {
      close_callback();
    } catch (const std::exception &e) {
      MCP_LOG_ERROR("Exception in close callback: " + std::string(e.what()));
    }
  }

  MCP_LOG_INFO("StdioTransport disconnected");
}

bool StdioTransport::isConnected() const { return running_; }

void StdioTransport::readLoop() {
  try {
    std::string line;
    while (running_ && std::getline(std::cin, line)) {
      if (!running_) {
        break;
      }

      try {
        processLine(line);
      } catch (const std::exception &e) {
        MCP_LOG_ERROR("Error processing line: " + std::string(e.what()));

        std::function<void(std::error_code)> error_callback;
        {
          std::lock_guard<std::mutex> lock(callback_mutex_);
          error_callback = error_callback_;
        }

        if (error_callback) {
          try {
            error_callback(make_error_code(StdioError::ReadError));
          } catch (const std::exception &e) {
            MCP_LOG_ERROR("Exception in error callback: " +
                          std::string(e.what()));
          }
        }
      }
    }
  } catch (const std::exception &e) {
    MCP_LOG_ERROR("Error in read loop: " + std::string(e.what()));

    std::function<void(std::error_code)> error_callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      error_callback = error_callback_;
    }

    if (error_callback) {
      try {
        error_callback(make_error_code(StdioError::ReadError));
      } catch (const std::exception &e) {
        MCP_LOG_ERROR("Exception in error callback: " + std::string(e.what()));
      }
    }
  }

  // We're done reading, so we'll disconnect
  if (running_) {
    disconnect();
  }
}

void StdioTransport::writeLoop() {
  try {
    while (running_) {
      SendOperation op;
      {
        std::unique_lock<std::mutex> lock(send_queue_mutex_);

        // Wait for a message to send or for shutdown
        send_queue_cv_.wait(
            lock, [this] { return !running_ || !send_queue_.empty(); });

        if (!running_) {
          break;
        }

        op = send_queue_.front();
        send_queue_.pop();
      }

      try {
        // Serialize the message
        std::string json = json_utils::serializeMessage(op.message);

        // Write to stdout
        std::cout << json << std::endl;

        // Invoke the callback with success
        if (op.callback) {
          op.callback({});
        }
      } catch (const std::exception &e) {
        MCP_LOG_ERROR("Error sending message: " + std::string(e.what()));

        // Invoke the callback with error
        if (op.callback) {
          op.callback(make_error_code(StdioError::WriteError));
        }

        std::function<void(std::error_code)> error_callback;
        {
          std::lock_guard<std::mutex> lock(callback_mutex_);
          error_callback = error_callback_;
        }

        if (error_callback) {
          try {
            error_callback(make_error_code(StdioError::WriteError));
          } catch (const std::exception &e) {
            MCP_LOG_ERROR("Exception in error callback: " +
                          std::string(e.what()));
          }
        }
      }
    }
  } catch (const std::exception &e) {
    MCP_LOG_ERROR("Error in write loop: " + std::string(e.what()));

    std::function<void(std::error_code)> error_callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      error_callback = error_callback_;
    }

    if (error_callback) {
      try {
        error_callback(make_error_code(StdioError::WriteError));
      } catch (const std::exception &e) {
        MCP_LOG_ERROR("Exception in error callback: " + std::string(e.what()));
      }
    }
  }
}

void StdioTransport::processLine(const std::string &line) {
  MCP_LOG_DEBUG("Received: " + line);

  // Parse the message
  types::JSONRPCMessage message = json_utils::parseMessage(line);

  // Invoke the callback
  std::function<void(types::JSONRPCMessage)> message_callback;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    message_callback = message_callback_;
  }

  if (message_callback) {
    try {
      message_callback(message);
    } catch (const std::exception &e) {
      MCP_LOG_ERROR("Exception in message callback: " + std::string(e.what()));
    }
  }
}

} // namespace transport
} // namespace mcp