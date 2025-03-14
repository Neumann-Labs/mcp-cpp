#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "mcp/session/client_session.hpp"
#include "mcp/session/server_session.hpp"
#include "mcp/session/session.hpp"
#include "mcp/types.hpp"
#include "mcp/utils/error.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

// Mock transport for testing
class MockTransport : public mcp::transport::Transport {
public:
  MOCK_METHOD(void, send,
              (const mcp::types::JSONRPCMessage &,
               std::function<void(const std::error_code &)>),
              (override));
  MOCK_METHOD(std::error_code, send,
              (const mcp::types::JSONRPCMessage &, std::chrono::milliseconds),
              (override));
  MOCK_METHOD(void, setMessageCallback,
              (std::function<void(mcp::types::JSONRPCMessage)>), (override));
  MOCK_METHOD(void, setErrorCallback, (std::function<void(std::error_code)>),
              (override));
  MOCK_METHOD(void, setCloseCallback, (std::function<void()>), (override));
  MOCK_METHOD(void, connect, (), (override));
  MOCK_METHOD(void, disconnect, (), (override));
  MOCK_METHOD(bool, isConnected, (), (const, override));
};

class AdvancedSessionTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a mock transport
    transport_ = std::make_shared<testing::NiceMock<MockTransport>>();

    // Allow the transport to be "connected" by default
    ON_CALL(*transport_, isConnected()).WillByDefault(testing::Return(true));

    // By default, the sync send succeeds
    ON_CALL(*transport_,
            send(testing::_, testing::An<std::chrono::milliseconds>()))
        .WillByDefault(testing::Return(std::error_code()));

    // Capture message callbacks for testing
    ON_CALL(*transport_, setMessageCallback(testing::_))
        .WillByDefault(
            [this](std::function<void(mcp::types::JSONRPCMessage)> callback) {
              message_callback_ = callback;
            });

    ON_CALL(*transport_, setErrorCallback(testing::_))
        .WillByDefault([this](std::function<void(std::error_code)> callback) {
          error_callback_ = callback;
        });

    ON_CALL(*transport_, setCloseCallback(testing::_))
        .WillByDefault([this](std::function<void()> callback) {
          close_callback_ = callback;
        });
  }

  // Call these to simulate receiving messages, errors, or close
  void simulateMessageReceived(const mcp::types::JSONRPCMessage &message) {
    if (message_callback_) {
      message_callback_(message);
    }
  }

  void simulateError(const std::error_code &error) {
    if (error_callback_) {
      error_callback_(error);
    }
  }

  void simulateClose() {
    if (close_callback_) {
      close_callback_();
    }
  }

  // Helper to create a response for a request
  mcp::types::JSONRPCResponse createResponse(const std::string &id,
                                             const nlohmann::json &result) {
    mcp::types::JSONRPCResponse response;
    response.jsonrpc = "2.0";
    response.id = id;
    response.result = result;
    return response;
  }

  std::shared_ptr<MockTransport> transport_;
  std::function<void(mcp::types::JSONRPCMessage)> message_callback_;
  std::function<void(std::error_code)> error_callback_;
  std::function<void()> close_callback_;
};

// Test for request timeout
TEST_F(AdvancedSessionTest, RequestTimeout) {
  mcp::Session session(transport_);

  // Connect the session
  session.connect();

  // Capture the request ID
  std::string request_id;

  ON_CALL(*transport_,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([&request_id](const mcp::types::JSONRPCMessage &message,
                                   std::chrono::milliseconds) {
        if (std::holds_alternative<mcp::types::JSONRPCRequest>(message)) {
          const auto &request = std::get<mcp::types::JSONRPCRequest>(message);
          if (std::holds_alternative<std::string>(request.id)) {
            request_id = std::get<std::string>(request.id);
          } else if (std::holds_alternative<int>(request.id)) {
            request_id = std::to_string(std::get<int>(request.id));
          }
        }
        return std::error_code();
      });

  // Send a request with a very short timeout
  auto future = session.sendRequest("test_method", std::nullopt,
                                    std::chrono::milliseconds(50));

  // Wait for the timeout to occur
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // The future should be ready due to timeout
  EXPECT_EQ(future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);

  // Verify the error is a timeout
  try {
    future.get();
    FAIL() << "Expected a timeout exception";
  } catch (const mcp::utils::TimeoutException &) {
    // Expected timeout exception
  } catch (const std::exception &e) {
    FAIL() << "Expected a TimeoutException, got: " << e.what();
  }
}

// Test for concurrent requests
TEST_F(AdvancedSessionTest, ConcurrentRequests) {
  mcp::Session session(transport_);

  // Connect the session
  session.connect();

  // Capture request IDs
  std::mutex id_mutex;
  std::vector<std::string> request_ids;

  ON_CALL(*transport_,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([&id_mutex,
                      &request_ids](const mcp::types::JSONRPCMessage &message,
                                    std::chrono::milliseconds) {
        if (std::holds_alternative<mcp::types::JSONRPCRequest>(message)) {
          const auto &request = std::get<mcp::types::JSONRPCRequest>(message);
          std::string id;
          if (std::holds_alternative<std::string>(request.id)) {
            id = std::get<std::string>(request.id);
          } else if (std::holds_alternative<int>(request.id)) {
            id = std::to_string(std::get<int>(request.id));
          }

          std::lock_guard<std::mutex> lock(id_mutex);
          request_ids.push_back(id);
        }
        return std::error_code();
      });

  // Send multiple requests concurrently
  constexpr int NUM_REQUESTS = 10;
  std::vector<std::future<mcp::types::JSONRPCResponse>> futures;

  for (int i = 0; i < NUM_REQUESTS; i++) {
    futures.push_back(
        session.sendRequest("test_method", nlohmann::json({{"index", i}})));
  }

  // Wait a bit for all requests to be sent
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify that all requests were sent
  {
    std::lock_guard<std::mutex> lock(id_mutex);
    EXPECT_EQ(request_ids.size(), NUM_REQUESTS);
  }

  // Send responses for all requests
  {
    std::lock_guard<std::mutex> lock(id_mutex);
    for (int i = 0; i < NUM_REQUESTS; i++) {
      auto response = createResponse(request_ids[i], {{"result", i}});
      simulateMessageReceived(response);
    }
  }

  // Verify that all futures are ready
  for (int i = 0; i < NUM_REQUESTS; i++) {
    EXPECT_EQ(futures[i].wait_for(std::chrono::seconds(0)),
              std::future_status::ready);

    // Verify the response
    auto result = futures[i].get();
    EXPECT_EQ(result.result["result"], i);
  }
}

// Test for message stream ordering
TEST_F(AdvancedSessionTest, MessageStreamOrdering) {
  mcp::Session session(transport_);

  // Connect the session
  session.connect();

  // Set up notification handler
  std::mutex notifications_mutex;
  std::vector<int> notifications;

  session.setNotificationHandler(
      [&notifications_mutex,
       &notifications](const mcp::types::JSONRPCNotification &notification) {
        if (notification.params.has_value() &&
            notification.params->contains("index")) {
          std::lock_guard<std::mutex> lock(notifications_mutex);
          notifications.push_back(notification.params->at("index").get<int>());
        }
      });

  // Send multiple notifications in a specific order
  constexpr int NUM_NOTIFICATIONS = 10;

  // Add a short delay after registration to ensure the handler is properly set
  // up
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  for (int i = 0; i < NUM_NOTIFICATIONS; i++) {
    mcp::types::JSONRPCNotification notification;
    notification.jsonrpc = "2.0";
    notification.method = "test_notification";
    notification.params = {{"index", i}};

    simulateMessageReceived(notification);

    // Small delay to ensure notifications are processed
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Give some time for all notifications to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify that notifications were processed in order
  std::lock_guard<std::mutex> lock(notifications_mutex);
  ASSERT_EQ(notifications.size(), NUM_NOTIFICATIONS);

  for (int i = 0; i < NUM_NOTIFICATIONS; i++) {
    EXPECT_EQ(notifications[i], i);
  }
}

// Test for session reconnection
TEST_F(AdvancedSessionTest, SessionReconnection) {
  mcp::Session session(transport_);

  // Connect the session
  session.connect();

  // Send a request
  auto future = session.sendRequest("test_method");

  // Simulate disconnection
  simulateClose();

  // Verify the session is disconnected
  EXPECT_FALSE(session.isConnected());

  // The pending request should be failed
  EXPECT_EQ(future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);

  try {
    future.get();
    FAIL() << "Expected an exception";
  } catch (const mcp::utils::TransportException &) {
    // Expected exception
  }

  // Reconnect the session
  session.connect();

  // Verify the session is connected again
  EXPECT_TRUE(session.isConnected());

  // New requests should work again
  std::string request_id;

  ON_CALL(*transport_,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([&request_id](const mcp::types::JSONRPCMessage &message,
                                   std::chrono::milliseconds) {
        if (std::holds_alternative<mcp::types::JSONRPCRequest>(message)) {
          const auto &request = std::get<mcp::types::JSONRPCRequest>(message);
          if (std::holds_alternative<std::string>(request.id)) {
            request_id = std::get<std::string>(request.id);
          } else if (std::holds_alternative<int>(request.id)) {
            request_id = std::to_string(std::get<int>(request.id));
          }
        }
        return std::error_code();
      });

  auto new_future = session.sendRequest("test_method");

  // Send a response
  auto response = createResponse(request_id, {{"result", "success"}});
  simulateMessageReceived(response);

  // Verify the future is ready
  EXPECT_EQ(new_future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);

  // Verify the response
  auto result = new_future.get();
  EXPECT_EQ(result.result["result"], "success");
}

// Test for request cancellation
TEST_F(AdvancedSessionTest, RequestCancellation) {
  mcp::Session session(transport_);

  // Connect the session
  session.connect();

  // Set up a condition to signal when the request is sent
  std::mutex mutex;
  std::condition_variable cv;
  bool request_sent = false;
  std::string request_id;

  // Mock send to capture the request ID and signal when sent
  ON_CALL(*transport_,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([&](const mcp::types::JSONRPCMessage &message,
                         std::chrono::milliseconds) {
        if (std::holds_alternative<mcp::types::JSONRPCRequest>(message)) {
          const auto &request = std::get<mcp::types::JSONRPCRequest>(message);
          if (std::holds_alternative<std::string>(request.id)) {
            request_id = std::get<std::string>(request.id);
          } else if (std::holds_alternative<int>(request.id)) {
            request_id = std::to_string(std::get<int>(request.id));
          }

          // Signal that the request was sent
          {
            std::lock_guard<std::mutex> lock(mutex);
            request_sent = true;
          }
          cv.notify_one();
        }
        return std::error_code();
      });

  // Send a request, but don't wait for it to complete
  auto future = session.sendRequest("test_method");

  // Wait for the request to be sent
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&request_sent]() { return request_sent; });
  }

  // Verify that the request ID was captured
  EXPECT_FALSE(request_id.empty());

  // Disconnect the session (which should cancel the request)
  session.disconnect();

  // Verify the future is ready (due to cancellation)
  EXPECT_EQ(future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);

  // Verify the error is a transport error
  try {
    future.get();
    FAIL() << "Expected an exception";
  } catch (const mcp::utils::TransportException &e) {
    EXPECT_EQ(e.error().code,
              static_cast<int>(mcp::types::ErrorCode::TransportError));
  } catch (...) {
    FAIL() << "Expected a TransportException";
  }
}

// Test for response to unknown request
TEST_F(AdvancedSessionTest, ResponseToUnknownRequest) {
  mcp::Session session(transport_);

  // Connect the session
  session.connect();

  // Capture log messages
  bool warning_logged = false;

  // Monitor logs to verify warning is logged
  // This would be better with a mock logger, but for simplicity we'll just
  // check the behavior In a real implementation, you'd mock the logging system

  // Send a response for an unknown request ID
  mcp::types::JSONRPCResponse response;
  response.jsonrpc = "2.0";
  response.id = "unknown-id";
  response.result = {{"result", "success"}};

  simulateMessageReceived(response);

  // We can't directly verify the warning was logged without mocking the logging
  // system, but the session should continue functioning properly

  // Send a valid request and verify it works
  std::string request_id;

  ON_CALL(*transport_,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([&request_id](const mcp::types::JSONRPCMessage &message,
                                   std::chrono::milliseconds) {
        if (std::holds_alternative<mcp::types::JSONRPCRequest>(message)) {
          const auto &request = std::get<mcp::types::JSONRPCRequest>(message);
          if (std::holds_alternative<std::string>(request.id)) {
            request_id = std::get<std::string>(request.id);
          } else if (std::holds_alternative<int>(request.id)) {
            request_id = std::to_string(std::get<int>(request.id));
          }
        }
        return std::error_code();
      });

  auto future = session.sendRequest("test_method");

  // Send a response
  auto valid_response = createResponse(request_id, {{"result", "success"}});
  simulateMessageReceived(valid_response);

  // Verify the future is ready
  EXPECT_EQ(future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);

  // Verify the response
  auto result = future.get();
  EXPECT_EQ(result.result["result"], "success");
}

// Additional tests for ServerSession

class AdvancedServerSessionTest : public AdvancedSessionTest {
protected:
  void SetUp() override {
    AdvancedSessionTest::SetUp();

    // Create server info
    server_info_.name = "test-server";
    server_info_.version = "1.0.0";
    server_info_.instructions = "Test server instructions";
  }

  mcp::types::ServerInfo server_info_;
};

// Test for server error response handling
TEST_F(AdvancedServerSessionTest, ErrorResponseHandling) {
  // Create a server session
  mcp::ServerSession session(transport_, server_info_);

  // Connect the session
  session.connect();

  // Simulate an initialize request
  mcp::types::JSONRPCRequest init_request;
  init_request.jsonrpc = "2.0";
  init_request.id = "init-id";
  init_request.method = "initialize";
  init_request.params = {
      {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};

  // Initialize response will be sent
  EXPECT_CALL(*transport_,
              send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillOnce(testing::Return(std::error_code()));

  simulateMessageReceived(init_request);

  // Store the message sent by the session for verification
  mcp::types::JSONRPCMessage captured_message;

  // Expect an error response for a request with missing parameters
  EXPECT_CALL(*transport_,
              send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillOnce([&captured_message](const mcp::types::JSONRPCMessage &message,
                                    std::chrono::milliseconds) {
        captured_message = message;
        return std::error_code();
      });

  // Simulate a callTool request with missing parameters
  mcp::types::JSONRPCRequest tool_request;
  tool_request.jsonrpc = "2.0";
  tool_request.id = "tool-id";
  tool_request.method = "callTool";
  // Missing params

  simulateMessageReceived(tool_request);

  // Wait a moment for the message to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify the response was an error
  EXPECT_TRUE(
      std::holds_alternative<mcp::types::JSONRPCError>(captured_message));
  if (std::holds_alternative<mcp::types::JSONRPCError>(captured_message)) {
    const auto &error = std::get<mcp::types::JSONRPCError>(captured_message);
    EXPECT_EQ(error.jsonrpc, "2.0");
    EXPECT_EQ(error.error.code,
              static_cast<int>(mcp::types::ErrorCode::InvalidParams));
  }
}

// Test for server notification sending
TEST_F(AdvancedServerSessionTest, ServerNotificationSending) {
  // Create a server session
  mcp::ServerSession session(transport_, server_info_);

  // Connect the session
  session.connect();

  // Initialize the session
  mcp::types::JSONRPCRequest init_request;
  init_request.jsonrpc = "2.0";
  init_request.id = "init-id";
  init_request.method = "initialize";
  init_request.params = {
      {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};

  // Initialize response will be sent
  EXPECT_CALL(*transport_,
              send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillOnce(testing::Return(std::error_code()));

  simulateMessageReceived(init_request);

  // Store the notification message for verification
  mcp::types::JSONRPCMessage captured_message;

  // Expect a progress notification to be sent
  EXPECT_CALL(*transport_,
              send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillOnce([&captured_message](const mcp::types::JSONRPCMessage &message,
                                    std::chrono::milliseconds) {
        captured_message = message;
        return std::error_code();
      });

  // Send a notification from the server
  session.sendNotification("progress", {{"progress", 0.5}});

  // Wait a moment for the notification to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify the notification
  EXPECT_TRUE(std::holds_alternative<mcp::types::JSONRPCNotification>(
      captured_message));
  if (std::holds_alternative<mcp::types::JSONRPCNotification>(
          captured_message)) {
    const auto &notification =
        std::get<mcp::types::JSONRPCNotification>(captured_message);
    EXPECT_EQ(notification.jsonrpc, "2.0");
    EXPECT_EQ(notification.method, "progress");
    EXPECT_TRUE(notification.params.has_value());

    // Check that progress is in the params
    EXPECT_TRUE(notification.params->contains("progress"));
  }
}

// Test for resource registration and access
TEST_F(AdvancedServerSessionTest, ResourceRegistrationAndAccess) {
  // Create a server session
  mcp::ServerSession session(transport_, server_info_);

  // Flag to track if the resource handler was called
  bool resource_called = false;

  // Register a resource
  session.registerResource({"test-uri", "Test Resource", "A test resource"},
                           [&resource_called](const std::string &uri)
                               -> mcp::types::ReadResourceResult {
                             resource_called = true;
                             EXPECT_EQ(uri, "test-uri");
                             // Create a properly initialized resource result
                             mcp::types::ReadResourceResult result;
                             result.content = "Test content";
                             result.contentType = "text/plain";
                             return result;
                           });

  // Connect the session
  session.connect();

  // Initialize the session
  mcp::types::JSONRPCRequest init_request;
  init_request.jsonrpc = "2.0";
  init_request.id = "init-id";
  init_request.method = "initialize";
  init_request.params = {
      {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};

  // Initialize response will be sent
  EXPECT_CALL(*transport_,
              send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillOnce(testing::Return(std::error_code()));

  simulateMessageReceived(init_request);

  // Capture list response for verification
  mcp::types::JSONRPCMessage list_response;

  // Expect a resource list response
  EXPECT_CALL(*transport_,
              send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillOnce([&list_response](const mcp::types::JSONRPCMessage &message,
                                 std::chrono::milliseconds) {
        list_response = message;
        return std::error_code();
      });

  // Simulate a listResources request
  mcp::types::JSONRPCRequest list_request;
  list_request.jsonrpc = "2.0";
  list_request.id = "list-id";
  list_request.method = "listResources";

  simulateMessageReceived(list_request);

  // Wait for the list response to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify the list response
  EXPECT_TRUE(
      std::holds_alternative<mcp::types::JSONRPCResponse>(list_response));
  if (std::holds_alternative<mcp::types::JSONRPCResponse>(list_response)) {
    const auto &response = std::get<mcp::types::JSONRPCResponse>(list_response);
    EXPECT_EQ(response.jsonrpc, "2.0");
    EXPECT_TRUE(response.result.contains("resources"));
    EXPECT_TRUE(response.result["resources"].is_array());
    EXPECT_FALSE(response.result["resources"].empty());
    EXPECT_EQ(response.result["resources"][0]["uri"], "test-uri");
  }

  // Capture read response for verification
  mcp::types::JSONRPCMessage read_response;

  // Expect a readResource response
  EXPECT_CALL(*transport_,
              send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillOnce([&read_response](const mcp::types::JSONRPCMessage &message,
                                 std::chrono::milliseconds) {
        read_response = message;
        return std::error_code();
      });

  // Simulate a readResource request
  mcp::types::JSONRPCRequest read_request;
  read_request.jsonrpc = "2.0";
  read_request.id = "read-id";
  read_request.method = "readResource";
  read_request.params = {{"uri", "test-uri"}};

  simulateMessageReceived(read_request);

  // Wait for the read response to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify the read response
  EXPECT_TRUE(
      std::holds_alternative<mcp::types::JSONRPCResponse>(read_response));
  if (std::holds_alternative<mcp::types::JSONRPCResponse>(read_response)) {
    const auto &response = std::get<mcp::types::JSONRPCResponse>(read_response);
    EXPECT_EQ(response.jsonrpc, "2.0");
    EXPECT_TRUE(response.result.contains("content"));
    EXPECT_EQ(response.result["content"], "Test content");
    EXPECT_TRUE(response.result.contains("contentType"));
    EXPECT_EQ(response.result["contentType"], "text/plain");
  }

  // Verify the resource handler was called
  EXPECT_TRUE(resource_called);
}

// Additional tests for ClientSession

class AdvancedClientSessionTest : public AdvancedSessionTest {
protected:
  void SetUp() override { AdvancedSessionTest::SetUp(); }

  // Helper to set up a connected and initialized client session
  std::shared_ptr<mcp::ClientSession> setupInitializedClientSession() {
    auto session = std::make_shared<mcp::ClientSession>(transport_);

    // Connect the session
    session->connect();

    // Capture the request ID and simulate a response
    std::string request_id;

    ON_CALL(*transport_,
            send(testing::_, testing::An<std::chrono::milliseconds>()))
        .WillByDefault([&request_id](const mcp::types::JSONRPCMessage &message,
                                     std::chrono::milliseconds) {
          if (std::holds_alternative<mcp::types::JSONRPCRequest>(message)) {
            const auto &request = std::get<mcp::types::JSONRPCRequest>(message);
            if (std::holds_alternative<std::string>(request.id)) {
              request_id = std::get<std::string>(request.id);
            } else if (std::holds_alternative<int>(request.id)) {
              request_id = std::to_string(std::get<int>(request.id));
            }
          }
          return std::error_code();
        });

    // Prepare client info
    mcp::types::ClientInfo client_info;
    client_info.name = "test-client";
    client_info.version = "1.0.0";

    // Start initialization
    auto future = session->initialize(client_info);

    // Simulate an initialization response
    // Use direct fields instead of nested objects to match what ClientSession
    // now expects
    mcp::types::JSONRPCResponse response;
    response.jsonrpc = "2.0";
    response.id = request_id;
    response.result = {
        {"serverName", "test-server"},
        {"serverVersion", "1.0.0"},
        {"instructions", "Test server instructions"},
        {"capabilities", {{"tools", {{"supportsStreaming", true}}}}}};

    simulateMessageReceived(response);

    // Wait for initialization to complete with a timeout
    auto status = future.wait_for(std::chrono::seconds(1));
    if (status != std::future_status::ready) {
      throw std::runtime_error("Initialization timed out");
    }

    return session;
  }
};

// Test for client tool discovery
TEST_F(AdvancedClientSessionTest, ToolDiscovery) {
  auto session = setupInitializedClientSession();

  // Capture the request ID
  std::string request_id;

  ON_CALL(*transport_,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([&request_id](const mcp::types::JSONRPCMessage &message,
                                   std::chrono::milliseconds) {
        if (std::holds_alternative<mcp::types::JSONRPCRequest>(message)) {
          const auto &request = std::get<mcp::types::JSONRPCRequest>(message);
          if (std::holds_alternative<std::string>(request.id)) {
            request_id = std::get<std::string>(request.id);
          } else if (std::holds_alternative<int>(request.id)) {
            request_id = std::to_string(std::get<int>(request.id));
          }
        }
        return std::error_code();
      });

  // Call listTools
  auto future = session->listTools();

  // Simulate a listTools response
  mcp::types::JSONRPCResponse response;
  response.jsonrpc = "2.0";
  response.id = request_id;
  response.result = {
      {"tools",
       {{{"name", "tool1"},
         {"description", "Tool 1 description"},
         {"inputSchema", {{"type", "object"}}}},
        {{"name", "tool2"}, {"description", "Tool 2 description"}}}}};

  simulateMessageReceived(response);

  // Wait a bit for the message to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify the future is ready
  EXPECT_EQ(future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);

  // Verify the response
  auto result = future.get();
  EXPECT_EQ(result.tools.size(), 2);
  EXPECT_EQ(result.tools[0].name, "tool1");
  EXPECT_EQ(result.tools[0].description, "Tool 1 description");
  EXPECT_TRUE(result.tools[0].input_schema.has_value());
  EXPECT_EQ(result.tools[1].name, "tool2");
  EXPECT_EQ(result.tools[1].description, "Tool 2 description");
  EXPECT_FALSE(result.tools[1].input_schema.has_value());
}

// Test for client tool invocation
TEST_F(AdvancedClientSessionTest, ToolInvocation) {
  auto session = setupInitializedClientSession();

  // Capture the request ID
  std::string request_id;

  ON_CALL(*transport_,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([&request_id](const mcp::types::JSONRPCMessage &message,
                                   std::chrono::milliseconds) {
        if (std::holds_alternative<mcp::types::JSONRPCRequest>(message)) {
          const auto &request = std::get<mcp::types::JSONRPCRequest>(message);
          if (std::holds_alternative<std::string>(request.id)) {
            request_id = std::get<std::string>(request.id);
          } else if (std::holds_alternative<int>(request.id)) {
            request_id = std::to_string(std::get<int>(request.id));
          }

          // Verify the tool name and parameters
          if (request.method == "callTool" && request.params.has_value()) {
            const auto &params = request.params.value();
            EXPECT_EQ(params["name"], "test-tool");
            EXPECT_TRUE(params.contains("parameters"));
            EXPECT_EQ(params["parameters"]["param"], "value");
          }
        }
        return std::error_code();
      });

  // Call a tool
  auto future = session->callTool("test-tool", {{"param", "value"}});

  // Simulate a callTool response
  mcp::types::JSONRPCResponse response;
  response.jsonrpc = "2.0";
  response.id = request_id;
  response.result = {{"result", {{"key", "value"}, {"success", true}}}};

  simulateMessageReceived(response);

  // Wait a bit for the message to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Wait for the future to complete
  EXPECT_EQ(future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);

  // Verify the response
  auto result = future.get();
  EXPECT_TRUE(result.result.contains("key"));
  EXPECT_EQ(result.result["key"], "value");
  EXPECT_TRUE(result.result.contains("success"));
  EXPECT_TRUE(result.result["success"].get<bool>());
}

// Test for prompt template discovery and execution
TEST_F(AdvancedClientSessionTest, PromptDiscoveryAndExecution) {
  auto session = setupInitializedClientSession();

  // Capture the request ID
  std::string list_request_id;

  ON_CALL(*transport_,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([&list_request_id](
                         const mcp::types::JSONRPCMessage &message,
                         std::chrono::milliseconds) {
        if (std::holds_alternative<mcp::types::JSONRPCRequest>(message)) {
          const auto &request = std::get<mcp::types::JSONRPCRequest>(message);
          if (std::holds_alternative<std::string>(request.id)) {
            list_request_id = std::get<std::string>(request.id);
          } else if (std::holds_alternative<int>(request.id)) {
            list_request_id = std::to_string(std::get<int>(request.id));
          }
        }
        return std::error_code();
      });

  // Call listPrompts
  auto list_future = session->listPrompts();

  // Simulate a listPrompts response
  mcp::types::JSONRPCResponse list_response;
  list_response.jsonrpc = "2.0";
  list_response.id = list_request_id;
  list_response.result = {
      {"prompts",
       {{{"name", "greetings"},
         {"description", "Greeting prompt"},
         {"arguments",
          {{{"name", "name"}, {"description", "User name"}, {"required", true}},
           {{"name", "formal"},
            {"description", "Formal greeting"},
            {"required", false}}}}}}}};

  simulateMessageReceived(list_response);

  // Wait a bit for the message to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Wait for the future to complete
  EXPECT_EQ(list_future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);

  // Verify the response
  auto list_result = list_future.get();
  EXPECT_EQ(list_result.prompts.size(), 1);
  EXPECT_EQ(list_result.prompts[0].name, "greetings");
  EXPECT_EQ(list_result.prompts[0].description, "Greeting prompt");
  EXPECT_EQ(list_result.prompts[0].arguments.size(), 2);
  EXPECT_EQ(list_result.prompts[0].arguments[0].name, "name");
  EXPECT_TRUE(list_result.prompts[0].arguments[0].required);
  EXPECT_EQ(list_result.prompts[0].arguments[1].name, "formal");
  EXPECT_FALSE(list_result.prompts[0].arguments[1].required);

  // Capture the request ID for getPrompt
  std::string get_request_id;

  ON_CALL(*transport_,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([&get_request_id](
                         const mcp::types::JSONRPCMessage &message,
                         std::chrono::milliseconds) {
        if (std::holds_alternative<mcp::types::JSONRPCRequest>(message)) {
          const auto &request = std::get<mcp::types::JSONRPCRequest>(message);
          if (std::holds_alternative<std::string>(request.id)) {
            get_request_id = std::get<std::string>(request.id);
          } else if (std::holds_alternative<int>(request.id)) {
            get_request_id = std::to_string(std::get<int>(request.id));
          }
        }
        return std::error_code();
      });

  // Call getPrompt
  auto get_future =
      session->getPrompt("greetings", {{"name", "John"}, {"formal", true}});

  // Simulate a getPrompt response
  mcp::types::JSONRPCResponse get_response;
  get_response.jsonrpc = "2.0";
  get_response.id = get_request_id;
  get_response.result = {
      {"prompt", "Dear Mr. John, it is a pleasure to meet you."}};

  simulateMessageReceived(get_response);

  // Wait a bit for the message to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Wait for the future to complete
  EXPECT_EQ(get_future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);

  // Verify the response
  auto get_result = get_future.get();
  EXPECT_EQ(get_result.prompt, "Dear Mr. John, it is a pleasure to meet you.");
}