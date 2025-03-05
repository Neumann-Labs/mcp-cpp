#include "mcp/utils/error.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace mcp;
using namespace mcp::types;
using json = nlohmann::json;

// Test fixture for error handling tests
class ErrorTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Setup code if needed
  }

  void TearDown() override {
    // Teardown code if needed
  }
};

// Test MCPException
TEST_F(ErrorTest, MCPExceptionWithErrorData) {
  ErrorData error_data{.code = static_cast<int>(ErrorCode::InvalidParams),
                       .message = "Invalid parameters",
                       .data = json{{"param", "value"}}};

  MCPException exception(error_data);

  // Check exception properties
  EXPECT_EQ(exception.what(), std::string("Invalid parameters"));
  EXPECT_EQ(exception.error().code, static_cast<int>(ErrorCode::InvalidParams));
  EXPECT_EQ(exception.error().message, "Invalid parameters");
  EXPECT_EQ(exception.error().data["param"], "value");
}

// Test MCPException with error code
TEST_F(ErrorTest, MCPExceptionWithErrorCode) {
  MCPException exception(ErrorCode::InvalidRequest, "Invalid request");

  // Check exception properties
  EXPECT_EQ(exception.what(), std::string("Invalid request"));
  EXPECT_EQ(exception.error().code,
            static_cast<int>(ErrorCode::InvalidRequest));
  EXPECT_EQ(exception.error().message, "Invalid request");
  EXPECT_TRUE(exception.error().data.is_null());
}

// Test TransportException
TEST_F(ErrorTest, TransportException) {
  TransportException exception("Transport error",
                               json{{"details", "connection failed"}});

  // Check exception properties
  EXPECT_EQ(exception.what(), std::string("Transport error"));
  EXPECT_EQ(exception.error().code,
            static_cast<int>(ErrorCode::TransportError));
  EXPECT_EQ(exception.error().message, "Transport error");
  EXPECT_EQ(exception.error().data["details"], "connection failed");
}

// Test TransportException with error code
TEST_F(ErrorTest, TransportExceptionWithErrorCode) {
  TransportException exception(ErrorCode::TimeoutError, "Timeout",
                               json{{"timeout", 5000}});

  // Check exception properties
  EXPECT_EQ(exception.what(), std::string("Timeout"));
  EXPECT_EQ(exception.error().code, static_cast<int>(ErrorCode::TimeoutError));
  EXPECT_EQ(exception.error().message, "Timeout");
  EXPECT_EQ(exception.error().data["timeout"], 5000);
}

// Test ProtocolException
TEST_F(ErrorTest, ProtocolException) {
  ProtocolException exception("Protocol error",
                              json{{"details", "invalid message"}});

  // Check exception properties
  EXPECT_EQ(exception.what(), std::string("Protocol error"));
  EXPECT_EQ(exception.error().code, static_cast<int>(ErrorCode::ProtocolError));
  EXPECT_EQ(exception.error().message, "Protocol error");
  EXPECT_EQ(exception.error().data["details"], "invalid message");
}

// Test TimeoutException
TEST_F(ErrorTest, TimeoutException) {
  TimeoutException exception("Timeout", json{{"timeout", 5000}});

  // Check exception properties
  EXPECT_EQ(exception.what(), std::string("Timeout"));
  EXPECT_EQ(exception.error().code, static_cast<int>(ErrorCode::TimeoutError));
  EXPECT_EQ(exception.error().message, "Timeout");
  EXPECT_EQ(exception.error().data["timeout"], 5000);
}

// Test createErrorResponse from exception
TEST_F(ErrorTest, CreateErrorResponseFromException) {
  MCPException exception(ErrorCode::InvalidParams, "Invalid parameters");

  JSONRPCError error_response = createErrorResponse("request1", exception);

  // Check error response
  EXPECT_EQ(error_response.jsonrpc, "2.0");
  EXPECT_EQ(std::get<std::string>(error_response.id), "request1");
  EXPECT_EQ(error_response.error.code,
            static_cast<int>(ErrorCode::InvalidParams));
  EXPECT_EQ(error_response.error.message, "Invalid parameters");
}

// Test createErrorResponse from error data
TEST_F(ErrorTest, CreateErrorResponseFromErrorData) {
  ErrorData error_data{.code = static_cast<int>(ErrorCode::InvalidParams),
                       .message = "Invalid parameters",
                       .data = json{{"param", "value"}}};

  JSONRPCError error_response = createErrorResponse(123, error_data);

  // Check error response
  EXPECT_EQ(error_response.jsonrpc, "2.0");
  EXPECT_EQ(std::get<int>(error_response.id), 123);
  EXPECT_EQ(error_response.error.code,
            static_cast<int>(ErrorCode::InvalidParams));
  EXPECT_EQ(error_response.error.message, "Invalid parameters");
  EXPECT_EQ(error_response.error.data["param"], "value");
}

// Test createErrorResponse from error code and message
TEST_F(ErrorTest, CreateErrorResponseFromErrorCodeAndMessage) {
  JSONRPCError error_response =
      createErrorResponse("request1", ErrorCode::InvalidParams,
                          "Invalid parameters", json{{"param", "value"}});

  // Check error response
  EXPECT_EQ(error_response.jsonrpc, "2.0");
  EXPECT_EQ(std::get<std::string>(error_response.id), "request1");
  EXPECT_EQ(error_response.error.code,
            static_cast<int>(ErrorCode::InvalidParams));
  EXPECT_EQ(error_response.error.message, "Invalid parameters");
  EXPECT_EQ(error_response.error.data["param"], "value");
}