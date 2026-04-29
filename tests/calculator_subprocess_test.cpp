// SPDX-License-Identifier: Apache-2.0
//
// End-to-end test that spawns the `calculator_server` example as a real
// subprocess and drives it via pipes — proving the library works against
// itself across a process boundary using real stdio framing. The server
// path is plumbed in via the `MCP_CALCULATOR_SERVER` env var that
// CMakeLists.txt sets when registering this test.

#include "mcp/client.hpp"
#include "mcp/error.hpp"
#include "mcp/protocol.hpp"
#include "mcp/stdio_transport.hpp"

#include "test_binary_paths.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

extern char** environ;

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

struct Subprocess {
    pid_t pid          = -1;
    int   read_fd      = -1;   // server → us
    int   write_fd     = -1;   // us → server

    ~Subprocess() {
        if (write_fd >= 0) ::close(write_fd);
        if (read_fd  >= 0) ::close(read_fd);
        if (pid > 0) {
            int status = 0;
            ::kill(pid, SIGTERM);
            ::waitpid(pid, &status, 0);
        }
    }
};

// posix_spawn the calculator binary, returning pipes for stdio.
std::unique_ptr<Subprocess> spawn_calculator() {
#if !defined(MCP_TEST_CALCULATOR_SERVER)
    return nullptr;
#else
    const char* path = MCP_TEST_CALCULATOR_SERVER;

    int in_pipe[2];   // client writes to in_pipe[1], server reads in_pipe[0]
    int out_pipe[2];  // server writes to out_pipe[1], client reads out_pipe[0]
    if (::pipe(in_pipe) != 0)  return nullptr;
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        return nullptr;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // child stdin = in_pipe[0]
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0],  STDIN_FILENO);
    // child stdout = out_pipe[1]
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    // close the unused pipe ends in the child
    posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

    pid_t pid = -1;
    char* argv[] = { const_cast<char*>(path), nullptr };
    int rc = ::posix_spawn(&pid, path, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    // Close child-only ends in the parent.
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);

    if (rc != 0) {
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        return nullptr;
    }

    auto sp = std::make_unique<Subprocess>();
    sp->pid       = pid;
    sp->read_fd   = out_pipe[0];
    sp->write_fd  = in_pipe[1];
    return sp;
#endif
}

// -------------------------------------------------------------------------

TEST(CalculatorSubprocess, EndToEnd) {
#if !defined(MCP_TEST_CALCULATOR_SERVER)
    GTEST_SKIP() << "calculator_server not built; skipping subprocess test";
#else
    auto sp = spawn_calculator();
    ASSERT_TRUE(sp) << "spawn failed";

    mcp::StdioTransport::Options opts{};
    opts.read_fd  = sp->read_fd;
    opts.write_fd = sp->write_fd;
    opts.owns_fds = false;  // Subprocess dtor closes the parent ends
    auto transport = std::make_unique<mcp::StdioTransport>(opts);

    mcp::Client client{ mcp::Implementation{
        .name = "subprocess-tester", .version = "0.1.0",
    }};
    client.connect(std::move(transport));

    auto info = client.initialize().get();
    EXPECT_EQ(info.protocol_version, mcp::kLatestProtocolVersion);
    EXPECT_EQ(info.server_info.name, "mcp-cpp-calculator");
    ASSERT_TRUE(info.capabilities.tools.has_value());

    auto tools = client.list_tools().get();
    EXPECT_GE(tools.tools.size(), 4u);

    auto add_out = client.call_tool("add", json{{"a", 6.0}, {"b", 7.0}}).get();
    ASSERT_FALSE(add_out.is_error.value_or(false));
    ASSERT_EQ(add_out.content.size(), 1u);
    EXPECT_NE(std::get<mcp::TextContent>(add_out.content[0]).text.find("13"),
              std::string::npos);

    auto div_out = client.call_tool("divide", json{{"a", 1.0}, {"b", 0.0}}).get();
    EXPECT_TRUE(div_out.is_error.value_or(false));

    client.disconnect();
#endif
}

}  // namespace
