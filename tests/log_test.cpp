// SPDX-License-Identifier: Apache-2.0
#include "mcp/log.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CapturedLine {
    mcp::LogLevel level;
    std::string   message;
};

class LogTest : public ::testing::Test {
protected:
    void SetUp() override {
        prev_level_ = mcp::log_level();
        mcp::set_log_level(mcp::LogLevel::trace);
        mcp::set_log_sink([this](mcp::LogLevel lvl, std::string_view msg) {
            std::lock_guard<std::mutex> lk(mu_);
            captured_.push_back({lvl, std::string(msg)});
        });
    }

    void TearDown() override {
        mcp::set_log_sink(nullptr);  // reset to default sink
        mcp::set_log_level(prev_level_);
    }

    std::vector<CapturedLine> drain() {
        std::lock_guard<std::mutex> lk(mu_);
        return std::move(captured_);
    }

    std::mutex                mu_;
    std::vector<CapturedLine> captured_;
    mcp::LogLevel             prev_level_{mcp::LogLevel::info};
};

TEST_F(LogTest, EmitsAtAllLevels) {
    MCP_LOG_TRACE("t");
    MCP_LOG_DEBUG("d");
    MCP_LOG_INFO("i");
    MCP_LOG_WARN("w");
    MCP_LOG_ERROR("e");

    auto lines = drain();
    ASSERT_EQ(lines.size(), 5u);
    EXPECT_EQ(lines[0].level, mcp::LogLevel::trace);
    EXPECT_EQ(lines[0].message, "t");
    EXPECT_EQ(lines[4].level, mcp::LogLevel::error);
}

TEST_F(LogTest, LevelFilterDropsBelowThreshold) {
    mcp::set_log_level(mcp::LogLevel::warn);
    MCP_LOG_TRACE("t");
    MCP_LOG_DEBUG("d");
    MCP_LOG_INFO("i");
    MCP_LOG_WARN("w");
    MCP_LOG_ERROR("e");

    auto lines = drain();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].level, mcp::LogLevel::warn);
    EXPECT_EQ(lines[1].level, mcp::LogLevel::error);
}

TEST_F(LogTest, OffSilencesEverything) {
    mcp::set_log_level(mcp::LogLevel::off);
    MCP_LOG_ERROR("nope");
    EXPECT_TRUE(drain().empty());
}

TEST_F(LogTest, ConcurrentEmittersAreSerialized) {
    constexpr int kThreads = 8;
    constexpr int kPer     = 200;

    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t]() {
            for (int i = 0; i < kPer; ++i) {
                MCP_LOG_INFO(std::string("thread ") + std::to_string(t));
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(drain().size(), static_cast<std::size_t>(kThreads * kPer));
}

TEST_F(LogTest, ResettingSinkRestoresDefault) {
    mcp::set_log_sink(nullptr);  // restore default
    // Default writes to stderr; just verify the call doesn't crash.
    MCP_LOG_INFO("after-reset");
    SUCCEED();
}

TEST(LogLevelStrings, MapsAllValues) {
    EXPECT_EQ(mcp::to_string(mcp::LogLevel::trace), "TRACE");
    EXPECT_EQ(mcp::to_string(mcp::LogLevel::debug), "DEBUG");
    EXPECT_EQ(mcp::to_string(mcp::LogLevel::info),  "INFO");
    EXPECT_EQ(mcp::to_string(mcp::LogLevel::warn),  "WARN");
    EXPECT_EQ(mcp::to_string(mcp::LogLevel::error), "ERROR");
    EXPECT_EQ(mcp::to_string(mcp::LogLevel::off),   "OFF");
}

}  // namespace
