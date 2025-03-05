#include "mcp/utils/error.hpp"
#include "mcp/utils/json_utils.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace mcp;
using namespace mcp::json_utils;
using namespace mcp::types;
using json = nlohmann::json;

// Test fixture for JSON utilities tests
class JsonUtilsTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Setup code if needed
  }

  void TearDown() override {
    // Teardown code if needed
  }
};

// Test JSON parsing
TEST_F(JsonUtilsTest, ParseValidJson) {
  std::string json_str = R"({"key": "value", "number": 42})";

  json parsed = parse(json_str);

  EXPECT_EQ(parsed["key"], "value");
  EXPECT_EQ(parsed["number"], 42);
}

// Test JSON parsing with invalid JSON
TEST_F(JsonUtilsTest, ParseInvalidJson) {
  std::string json_str =
      R"({"key": "value", "number": 42)"; // Missing closing brace

  EXPECT_THROW(
      {
        try {
          json parsed = parse(json_str);
        } catch (const TransportException &e) {
          // Check that the exception has the correct code
          EXPECT_EQ(e.error().code,
                    static_cast<int>(ErrorCode::TransportError));
          throw;
        }
      },
      TransportException);
}

// Test JSON schema validation
TEST_F(JsonUtilsTest, ValidateValidJson) {
  json schema = R"({
    "type": "object",
    "required": ["name", "age"],
    "properties": {
      "name": {"type": "string"},
      "age": {"type": "integer", "minimum": 0}
    }
  })"_json;

  json valid_data = R"({
    "name": "John",
    "age": 30
  })"_json;

  json invalid_data = R"({
    "name": "John",
    "age": -5
  })"_json;

  // Test valid data
  std::string error_msg;
  bool valid = validate(valid_data, schema, &error_msg);
  EXPECT_TRUE(valid);
  EXPECT_TRUE(error_msg.empty());

  // Test invalid data
  valid = validate(invalid_data, schema, &error_msg);
  EXPECT_FALSE(valid);
  EXPECT_FALSE(error_msg.empty());
}

// Test message type detection
TEST_F(JsonUtilsTest, GetMessageType) {
  // Request
  json request = R"({
    "jsonrpc": "2.0",
    "id": 1,
    "method": "test.method",
    "params": {"param": "value"}
  })"_json;

  // Notification
  json notification = R"({
    "jsonrpc": "2.0",
    "method": "test.notification",
    "params": {"param": "value"}
  })"_json;

  // Response
  json response = R"({
    "jsonrpc": "2.0",
    "id": 1,
    "result": {"result": "value"}
  })"_json;

  // Error
  json error = R"({
    "jsonrpc": "2.0",
    "id": 1,
    "error": {
      "code": -32602,
      "message": "Invalid params"
    }
  })"_json;

  // Invalid (not JSON-RPC 2.0)
  json invalid = R"({
    "id": 1,
    "method": "test.method"
  })"_json;

  // Check message types
  EXPECT_EQ(getMessageType(request), MessageType::Request);
  EXPECT_EQ(getMessageType(notification), MessageType::Notification);
  EXPECT_EQ(getMessageType(response), MessageType::Response);
  EXPECT_EQ(getMessageType(error), MessageType::Error);

  // Check invalid message
  EXPECT_THROW({ getMessageType(invalid); }, ProtocolException);
}

// Test message parsing
TEST_F(JsonUtilsTest, ParseMessage) {
  std::string request_str = R"({
    "jsonrpc": "2.0",
    "id": 1,
    "method": "test.method",
    "params": {"param": "value"}
  })";

  types::JSONRPCMessage message = parseMessage(request_str);

  // Check that it's a request
  EXPECT_TRUE(std::holds_alternative<types::JSONRPCRequest>(message));

  types::JSONRPCRequest request = std::get<types::JSONRPCRequest>(message);
  EXPECT_EQ(request.jsonrpc, "2.0");
  EXPECT_EQ(std::get<int>(request.id), 1);
  EXPECT_EQ(request.method, "test.method");
  EXPECT_EQ((*request.params)["param"], "value");
}

// Test message serialization
TEST_F(JsonUtilsTest, SerializeMessage) {
  types::JSONRPCRequest request{.jsonrpc = "2.0",
                                .id = 1,
                                .method = "test.method",
                                .params = json{{"param", "value"}}};

  types::JSONRPCMessage message = request;
  std::string serialized = serializeMessage(message);

  // Parse back to JSON to check the structure
  json j = json::parse(serialized);

  EXPECT_EQ(j["jsonrpc"], "2.0");
  EXPECT_EQ(j["id"], 1);
  EXPECT_EQ(j["method"], "test.method");
  EXPECT_EQ(j["params"]["param"], "value");
}

// Test round-trip serialization/deserialization
TEST_F(JsonUtilsTest, RoundTripSerialization) {
  types::JSONRPCRequest original{.jsonrpc = "2.0",
                                 .id = 1,
                                 .method = "test.method",
                                 .params = json{{"param", "value"}}};

  types::JSONRPCMessage message = original;
  std::string serialized = serializeMessage(message);

  types::JSONRPCMessage parsed = parseMessage(serialized);
  EXPECT_TRUE(std::holds_alternative<types::JSONRPCRequest>(parsed));

  types::JSONRPCRequest deserialized = std::get<types::JSONRPCRequest>(parsed);

  EXPECT_EQ(original.jsonrpc, deserialized.jsonrpc);
  EXPECT_EQ(std::get<int>(original.id), std::get<int>(deserialized.id));
  EXPECT_EQ(original.method, deserialized.method);
  EXPECT_EQ((*original.params)["param"], (*deserialized.params)["param"]);
}