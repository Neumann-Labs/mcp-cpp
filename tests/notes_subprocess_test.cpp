// SPDX-License-Identifier: Apache-2.0
//
// End-to-end test that spawns the `mcp_notes_server` example as a real
// subprocess and drives it via pipes. Mirrors calculator_subprocess_test
// but exercises the SQLite/FTS5 demo: write → read → list → search →
// delete (no elicitation prompt for non-glob keys).

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

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <variant>

extern char** environ;

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

struct Subprocess {
    pid_t pid       = -1;
    int   read_fd   = -1;
    int   write_fd  = -1;

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

std::unique_ptr<Subprocess>
spawn_notes(const std::string& binary, const std::string& db_path) {
    int in_pipe[2];
    int out_pipe[2];
    if (::pipe(in_pipe)  != 0)  return nullptr;
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        return nullptr;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0],  STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

    pid_t pid = -1;
    char  arg0[]    = "mcp_notes_server";
    char  flag_db[] = "--db";
    std::string db  = db_path;  // mutable storage for argv
    char* argv[] = {
        const_cast<char*>(binary.c_str()),
        flag_db,
        db.data(),
        nullptr,
    };
    const int rc = ::posix_spawn(&pid, binary.c_str(), &actions, nullptr,
                                  argv, environ);
    (void)arg0;
    posix_spawn_file_actions_destroy(&actions);

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
}

TEST(NotesSubprocess, EndToEnd) {
#if !defined(MCP_TEST_NOTES_SERVER)
    GTEST_SKIP() << "mcp_notes_server not built (no SQLite3); skipping";
#else
    const char* path = MCP_TEST_NOTES_SERVER;

    // Use a unique tempfile per test so we don't collide with state
    // from a prior run.
    const auto db_path = (fs::temp_directory_path()
                          / ("mcp-notes-test-" + std::to_string(::getpid()) + ".db"))
                          .string();
    std::error_code ec;
    fs::remove(db_path, ec);

    auto sp = spawn_notes(path, db_path);
    ASSERT_TRUE(sp) << "spawn failed";

    mcp::StdioTransport::Options opts{};
    opts.read_fd  = sp->read_fd;
    opts.write_fd = sp->write_fd;
    opts.owns_fds = false;
    auto transport = std::make_unique<mcp::StdioTransport>(opts);

    mcp::Client client{ mcp::Implementation{
        .name = "notes-tester", .version = "0.1.0",
    }};
    client.connect(std::move(transport));

    auto info = client.initialize().get();
    EXPECT_EQ(info.server_info.name, "mcp-notes");
    ASSERT_TRUE(info.capabilities.tools.has_value());
    ASSERT_TRUE(info.capabilities.resources.has_value());

    auto tools = client.list_tools().get();
    EXPECT_GE(tools.tools.size(), 6u);  // write/read/list/search/delete/info

    // write a note
    {
        auto out = client.call_tool("notes/write", json{
            {"key",   "db.prod"},
            {"value", "postgres://prod.example/main"},
            {"tags",  "db,connection"},
        }).get();
        ASSERT_FALSE(out.is_error.value_or(false));
    }

    // read it back
    {
        auto out = client.call_tool("notes/read",
                                     json{{"key", "db.prod"}}).get();
        ASSERT_FALSE(out.is_error.value_or(false));
        EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text,
                  "postgres://prod.example/main");
    }

    // list — should see exactly one note
    {
        auto out = client.call_tool("notes/list", json{}).get();
        ASSERT_FALSE(out.is_error.value_or(false));
        ASSERT_TRUE(out.structured_content.has_value());
        EXPECT_EQ((*out.structured_content)["notes"].size(), 1u);
    }

    // search by content
    {
        auto out = client.call_tool("notes/search",
                                     json{{"query", "postgres"}}).get();
        ASSERT_FALSE(out.is_error.value_or(false));
        ASSERT_TRUE(out.structured_content.has_value());
        const auto& matches = (*out.structured_content)["matches"];
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_NE(matches[0]["snippet"].get<std::string>().find("postgres"),
                  std::string::npos);
    }

    // resource read
    {
        auto res = client.read_resource("notes://all").get();
        ASSERT_EQ(res.contents.size(), 1u);
        const auto& tc = std::get<mcp::TextResourceContents>(res.contents[0]);
        EXPECT_NE(tc.text.find("db.prod"), std::string::npos);
    }

    // delete with confirm=true (no elicitation needed)
    {
        auto out = client.call_tool("notes/delete", json{
            {"key", "db.prod"}, {"confirm", true},
        }).get();
        ASSERT_FALSE(out.is_error.value_or(false));
    }

    // info — count is back to zero
    {
        auto out = client.call_tool("notes/info", json{}).get();
        ASSERT_FALSE(out.is_error.value_or(false));
        ASSERT_TRUE(out.structured_content.has_value());
        EXPECT_EQ((*out.structured_content)["noteCount"].get<int>(), 0);
    }

    client.disconnect();
    fs::remove(db_path, ec);
#endif
}

TEST(NotesSubprocess, DeleteWithoutConfirmIsAbortedOnElicitDecline) {
#if !defined(MCP_TEST_NOTES_SERVER)
    GTEST_SKIP() << "mcp_notes_server not built; skipping";
#else
    // notes/delete with confirm omitted routes through elicitation;
    // declining the elicitation should leave the row in place.
    const char* path = MCP_TEST_NOTES_SERVER;

    const auto db_path = (fs::temp_directory_path()
                          / ("mcp-notes-elicit-" + std::to_string(::getpid()) + ".db"))
                          .string();
    std::error_code ec;
    fs::remove(db_path, ec);

    auto sp = spawn_notes(path, db_path);
    ASSERT_TRUE(sp);

    mcp::StdioTransport::Options opts{};
    opts.read_fd  = sp->read_fd;
    opts.write_fd = sp->write_fd;
    opts.owns_fds = false;
    auto transport = std::make_unique<mcp::StdioTransport>(opts);

    mcp::Client client{ mcp::Implementation{
        .name = "notes-tester", .version = "0.1.0",
    }};
    client.connect(std::move(transport));

    // Decline whatever the server elicits.
    client.set_elicitation_handler(
        [](const mcp::ElicitRequestParams&) -> mcp::ElicitResult {
            return mcp::ElicitResult{.action = mcp::ElicitAction::decline};
        });

    (void)client.initialize().get();

    (void)client.call_tool("notes/write", json{
        {"key", "k1"}, {"value", "v1"},
    }).get();

    auto del = client.call_tool("notes/delete",
                                  json{{"key", "k1"}}).get();
    EXPECT_TRUE(del.is_error.value_or(false));
    EXPECT_NE(std::get<mcp::TextContent>(del.content[0]).text.find("cancelled"),
              std::string::npos);

    // Note should still be there.
    auto info = client.call_tool("notes/info", json{}).get();
    ASSERT_TRUE(info.structured_content.has_value());
    EXPECT_EQ((*info.structured_content)["noteCount"].get<int>(), 1);

    client.disconnect();
    fs::remove(db_path, ec);
#endif
}

}  // namespace
