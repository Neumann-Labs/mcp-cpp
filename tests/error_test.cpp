// SPDX-License-Identifier: Apache-2.0
#include "mcp/error.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {

TEST(ErrorObject, DefaultConstructibleAndDataAbsent) {
    mcp::ErrorObject e{};
    EXPECT_EQ(e.code, 0);
    EXPECT_TRUE(e.message.empty());
    EXPECT_FALSE(e.has_data());
}

TEST(ErrorObject, HasDataReflectsNullness) {
    mcp::ErrorObject e{};
    e.data = nlohmann::json{{"k", 1}};
    EXPECT_TRUE(e.has_data());
}

TEST(ErrorException, CarriesCodeMessageAndData) {
    nlohmann::json extra = {{"why", "because"}};
    mcp::Error err{mcp::error_code::invalid_params, "bad arg", extra};
    EXPECT_EQ(err.code(), mcp::error_code::invalid_params);
    EXPECT_EQ(err.message(), "bad arg");
    EXPECT_EQ(err.data(), extra);
    EXPECT_STREQ(err.what(), "bad arg");
}

TEST(ErrorException, IsStdRuntimeError) {
    mcp::Error err{mcp::error_code::internal_error, "boom"};
    try {
        throw err;
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "boom");
    } catch (...) {
        FAIL() << "Error did not derive from std::runtime_error";
    }
}

TEST(ErrorException, ConstructFromObjectPreservesFields) {
    mcp::ErrorObject obj{mcp::error_code::method_not_found, "no such", {}};
    mcp::Error err{obj};
    EXPECT_EQ(err.code(), obj.code);
    EXPECT_EQ(err.message(), obj.message);
}

}  // namespace
