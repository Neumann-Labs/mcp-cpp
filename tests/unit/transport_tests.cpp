#include "mcp/transport/stdio_transport.hpp"
#include "mcp/utils/json_utils.hpp"
#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <thread>

using namespace mcp;
using namespace mcp::transport;
using namespace mcp::types;
using json = nlohmann::json;

// Test fixture for transport tests
class TransportTest : public ::testing::Test {
protected:
  // Since StdioTransport uses stdin/stdout, we can't directly test it in unit
  // tests We need to mock or create a testable version for proper unit testing
  // These tests mainly check the interface behavior and basic functionality

  void SetUp() override {
    // Setup code if needed
  }

  void TearDown() override {
    // Teardown code if needed
  }

  // Helper method to create a simple JSON-RPC request
  JSONRPCRequest createTestRequest(const std::string &method,
                                   const json &params = nullptr) {
    JSONRPCRequest request;
    request.jsonrpc = "2.0";
    request.id = 1;
    request.method = method;
    if (!params.is_null()) {
      request.params = params;
    }
    return request;
  }

  // Helper method to create a simple JSON-RPC notification
  JSONRPCNotification createTestNotification(const std::string &method,
                                             const json &params = nullptr) {
    JSONRPCNotification notification;
    notification.jsonrpc = "2.0";
    notification.method = method;
    if (!params.is_null()) {
      notification.params = params;
    }
    return notification;
  }
};

// Test basic transport creation
TEST_F(TransportTest, Creation) {
  // Simply test that we can create a StdioTransport instance
  StdioTransport transport;
  EXPECT_FALSE(transport.isConnected());
}

// Mock transport for testing without actual stdin/stdout
class MockTransport : public Transport {
public:
  MockTransport() : connected_(false) {}

  void send(const types::JSONRPCMessage &message,
            std::function<void(const std::error_code &)> callback) override {
    last_message_ = message;
    if (connected_ && callback) {
      callback({});
    } else if (callback) {
      callback(std::make_error_code(std::errc::not_connected));
    }
  }

  std::error_code send(const types::JSONRPCMessage &message,
                       std::chrono::milliseconds timeout) override {
    last_message_ = message;
    if (connected_) {
      return {};
    } else {
      return std::make_error_code(std::errc::not_connected);
    }
  }

  void setMessageCallback(
      std::function<void(types::JSONRPCMessage)> callback) override {
    message_callback_ = callback;
  }

  void
  setErrorCallback(std::function<void(std::error_code)> callback) override {
    error_callback_ = callback;
  }

  void setCloseCallback(std::function<void()> callback) override {
    close_callback_ = callback;
  }

  void connect() override { connected_ = true; }

  void disconnect() override {
    connected_ = false;
    if (close_callback_) {
      close_callback_();
    }
  }

  bool isConnected() const override { return connected_; }

  // Test helpers
  void simulateReceiveMessage(const types::JSONRPCMessage &message) {
    if (message_callback_) {
      message_callback_(message);
    }
  }

  void simulateError(const std::error_code &error) {
    if (error_callback_) {
      error_callback_(error);
    }
  }

  types::JSONRPCMessage getLastMessage() const { return last_message_; }

private:
  bool connected_;
  types::JSONRPCMessage last_message_;
  std::function<void(types::JSONRPCMessage)> message_callback_;
  std::function<void(std::error_code)> error_callback_;
  std::function<void()> close_callback_;
};

// Test sending a message
TEST_F(TransportTest, SendMessage) {
  MockTransport transport;
  transport.connect();

  // Create a request
  JSONRPCRequest request =
      createTestRequest("test.method", json{{"param", "value"}});
  JSONRPCMessage message = request;

  // Async send
  bool callback_called = false;
  transport.send(message, [&callback_called](const std::error_code &ec) {
    EXPECT_FALSE(ec);
    callback_called = true;
  });

  EXPECT_TRUE(callback_called);

  // Check that the message was sent
  JSONRPCMessage sent = transport.getLastMessage();
  EXPECT_TRUE(std::holds_alternative<JSONRPCRequest>(sent));

  JSONRPCRequest sent_request = std::get<JSONRPCRequest>(sent);
  EXPECT_EQ(sent_request.method, "test.method");
  EXPECT_EQ((*sent_request.params)["param"], "value");
}

// Test sending a message when disconnected
TEST_F(TransportTest, SendMessageDisconnected) {
  MockTransport transport;
  // Don't connect

  // Create a request
  JSONRPCRequest request = createTestRequest("test.method");
  JSONRPCMessage message = request;

  // Sync send with default timeout
  std::error_code ec = transport.send(message, std::chrono::milliseconds(5000));
  EXPECT_TRUE(ec);
  EXPECT_EQ(ec, std::make_error_code(std::errc::not_connected));
}

// Test receiving a message
TEST_F(TransportTest, ReceiveMessage) {
  MockTransport transport;
  transport.connect();

  // Set up a message callback
  bool callback_called = false;
  JSONRPCMessage received_message;

  transport.setMessageCallback([&](JSONRPCMessage message) {
    callback_called = true;
    received_message = message;
  });

  // Simulate receiving a message
  JSONRPCRequest request =
      createTestRequest("test.method", json{{"param", "value"}});
  transport.simulateReceiveMessage(request);

  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(std::holds_alternative<JSONRPCRequest>(received_message));

  JSONRPCRequest received_request = std::get<JSONRPCRequest>(received_message);
  EXPECT_EQ(received_request.method, "test.method");
  EXPECT_EQ((*received_request.params)["param"], "value");
}

// Test error callback
TEST_F(TransportTest, ErrorCallback) {
  MockTransport transport;
  transport.connect();

  // Set up an error callback
  bool callback_called = false;
  std::error_code received_error;

  transport.setErrorCallback([&](std::error_code ec) {
    callback_called = true;
    received_error = ec;
  });

  // Simulate an error
  std::error_code test_error =
      std::make_error_code(std::errc::connection_reset);
  transport.simulateError(test_error);

  EXPECT_TRUE(callback_called);
  EXPECT_EQ(received_error, test_error);
}

// Test close callback
TEST_F(TransportTest, CloseCallback) {
  MockTransport transport;
  transport.connect();

  // Set up a close callback
  bool callback_called = false;

  transport.setCloseCallback([&]() { callback_called = true; });

  // Disconnect
  transport.disconnect();

  EXPECT_TRUE(callback_called);
  EXPECT_FALSE(transport.isConnected());
}