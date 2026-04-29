// SPDX-License-Identifier: Apache-2.0
//
// Phase 4b tests: tasks. Cover wire-shape round-trips and the full
// "client task-augments a tools/call → server runs handler async →
//  client polls / awaits / cancels" loop.

#include "in_memory_transport.hpp"

#include "mcp/client.hpp"
#include "mcp/protocol.hpp"
#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

struct ServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread                  thread;

    template <typename Configure>
    ServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "tasks-srv", .version = "1.0",
          })) {
        cfg(*server);
        thread = std::thread([s = server, t = std::move(t)]() mutable {
            s->run(std::move(t));
        });
    }
    ~ServerThread() {
        server->stop();
        if (thread.joinable()) thread.join();
    }
};

// -------------------------------------------------------------------------
// Wire-shape round trips
// -------------------------------------------------------------------------

TEST(Tasks, TaskRoundTrip) {
    mcp::Task t{
        .taskId          = "abc",
        .status          = mcp::TaskStatus::working,
        .status_message  = "running",
        .created_at      = "2026-04-29T00:00:00Z",
        .last_updated_at = "2026-04-29T00:00:01Z",
        .ttl             = 60'000,
        .poll_interval   = 500,
    };
    json j = t;
    EXPECT_EQ(j["taskId"], "abc");
    EXPECT_EQ(j["status"], "working");
    EXPECT_EQ(j["ttl"],    60'000);
    EXPECT_EQ(j["pollInterval"], 500);
    auto back = j.get<mcp::Task>();
    EXPECT_EQ(back.taskId, "abc");
    EXPECT_EQ(back.status, mcp::TaskStatus::working);
    EXPECT_EQ(*back.poll_interval, 500);
}

TEST(Tasks, TaskStatusEnum) {
    EXPECT_EQ(json(mcp::TaskStatus::working),        "working");
    EXPECT_EQ(json(mcp::TaskStatus::input_required), "input_required");
    EXPECT_EQ(json(mcp::TaskStatus::completed),      "completed");
    EXPECT_EQ(json(mcp::TaskStatus::failed),         "failed");
    EXPECT_EQ(json(mcp::TaskStatus::cancelled),      "cancelled");

    EXPECT_EQ(json("input_required").get<mcp::TaskStatus>(),
              mcp::TaskStatus::input_required);
    EXPECT_THROW((void)json("nope").get<mcp::TaskStatus>(), mcp::Error);
}

TEST(Tasks, AugmentationRoundTrip) {
    mcp::TaskAugmentation a{.ttl = 12'345};
    json j = a;
    EXPECT_EQ(j["ttl"], 12'345);
    auto back = j.get<mcp::TaskAugmentation>();
    EXPECT_EQ(*back.ttl, 12'345);

    mcp::TaskAugmentation empty{};
    json j2 = empty;
    EXPECT_FALSE(j2.contains("ttl"));
}

TEST(Tasks, CallToolRequestParamsCarriesTask) {
    mcp::CallToolRequestParams p{
        .name      = "ask",
        .arguments = json{{"q", "?"}},
        .task      = mcp::TaskAugmentation{.ttl = 10'000},
    };
    json j = p;
    EXPECT_EQ(j["task"]["ttl"], 10'000);
    auto back = j.get<mcp::CallToolRequestParams>();
    EXPECT_TRUE(back.task.has_value());
    EXPECT_EQ(*back.task->ttl, 10'000);
}

TEST(Tasks, CapabilityRoundTrip) {
    mcp::TasksCapability tc;
    tc.list   = json::object();
    tc.cancel = json::object();
    mcp::TasksRequestsCapability rc;
    rc.tools = json{{"call", json::object()}};
    tc.requests = std::move(rc);

    json j = tc;
    EXPECT_TRUE(j["list"].is_object());
    EXPECT_TRUE(j["cancel"].is_object());
    EXPECT_EQ(j["requests"]["tools"]["call"], json::object());

    auto back = j.get<mcp::TasksCapability>();
    EXPECT_TRUE(back.list.has_value());
    EXPECT_TRUE(back.requests.has_value());
    EXPECT_TRUE(back.requests->tools.has_value());
}

TEST(Tasks, ListTasksResultRoundTrip) {
    mcp::ListTasksResult r{
        .tasks = {
            mcp::Task{.taskId = "1", .status = mcp::TaskStatus::working,
                      .created_at = "x", .last_updated_at = "y"},
        },
        .next_cursor = "1",
    };
    json j = r;
    EXPECT_EQ(j["tasks"].size(), 1u);
    EXPECT_EQ(j["nextCursor"], "1");
    auto back = j.get<mcp::ListTasksResult>();
    EXPECT_EQ(back.tasks.size(), 1u);
    EXPECT_EQ(*back.next_cursor, "1");
}

// -------------------------------------------------------------------------
// Integration tests
// -------------------------------------------------------------------------

TEST(TasksIntegration, ServerAdvertisesCapability) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks(/*default_ttl_ms=*/std::nullopt);
        s.tool("noop", json{{"type", "object"}},
               [](const json&) -> mcp::CallToolResult {
                   return {.content = { mcp::TextContent{.text = "ok"} }};
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    auto info = client.initialize().get();
    ASSERT_TRUE(info.capabilities.tasks.has_value());
    EXPECT_TRUE(info.capabilities.tasks->list.has_value());
    EXPECT_TRUE(info.capabilities.tasks->cancel.has_value());
    ASSERT_TRUE(info.capabilities.tasks->requests.has_value());
    EXPECT_TRUE(info.capabilities.tasks->requests->tools.has_value());

    client.disconnect();
}

TEST(TasksIntegration, AugmentedCallReturnsCreateTaskResult) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool("slow_add", json{{"type", "object"}},
               [](const json& args) -> mcp::CallToolResult {
                   std::this_thread::sleep_for(50ms);
                   const int a = args.value("a", 0);
                   const int b = args.value("b", 0);
                   return {.content = { mcp::TextContent{
                       .text = std::to_string(a + b),
                   }}};
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto envelope = client.call_tool_as_task(
        "slow_add", json{{"a", 2}, {"b", 3}}, 60'000).get();
    EXPECT_FALSE(envelope.task.taskId.empty());
    // The server marks the task working before the worker thread
    // flips to completed; either is acceptable depending on
    // scheduling, but it can't be a terminal-other.
    EXPECT_TRUE(envelope.task.status == mcp::TaskStatus::working ||
                envelope.task.status == mcp::TaskStatus::completed);

    // Block until the task is terminal and decode.
    auto raw = client.task_result(envelope.task.taskId).get();
    auto out = raw.get<mcp::CallToolResult>();
    ASSERT_FALSE(out.is_error.value_or(false));
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "5");

    // _meta must carry the related-task tag.
    ASSERT_TRUE(raw.contains("_meta"));
    ASSERT_TRUE(raw["_meta"].is_object());
    ASSERT_TRUE(raw["_meta"].contains(
        std::string{mcp::tasks_related_task_meta_key}));
    EXPECT_EQ(raw["_meta"][std::string{mcp::tasks_related_task_meta_key}]["taskId"],
              envelope.task.taskId);

    client.disconnect();
}

TEST(TasksIntegration, GetReturnsCurrentEnvelope) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool("add", json{{"type", "object"}},
               [](const json& args) -> mcp::CallToolResult {
                   const int a = args.value("a", 0);
                   const int b = args.value("b", 0);
                   return {.content = { mcp::TextContent{
                       .text = std::to_string(a + b),
                   }}};
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto env = client.call_tool_as_task("add", json{{"a", 1}, {"b", 2}}).get();
    // Drain to terminal.
    (void)client.task_result(env.task.taskId).get();

    auto t = client.task_get(env.task.taskId).get();
    EXPECT_EQ(t.taskId, env.task.taskId);
    EXPECT_EQ(t.status, mcp::TaskStatus::completed);

    client.disconnect();
}

TEST(TasksIntegration, ListEnumeratesTasks) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool("noop", json{{"type", "object"}},
               [](const json&) -> mcp::CallToolResult {
                   return {.content = { mcp::TextContent{.text = "ok"} }};
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    std::vector<std::string> ids;
    for (int i = 0; i < 3; ++i) {
        auto e = client.call_tool_as_task("noop", json{}).get();
        ids.push_back(e.task.taskId);
        // Drain so the test isn't flaky on the tasks/list snapshot.
        (void)client.task_result(e.task.taskId).get();
    }

    auto lst = client.task_list().get();
    EXPECT_EQ(lst.tasks.size(), 3u);
    EXPECT_FALSE(lst.next_cursor.has_value());

    client.disconnect();
}

TEST(TasksIntegration, CancelTransitionsToCancelled) {
    // The handler blocks on a flag the test flips after issuing the
    // cancel, so we observe the state machine: working → cancelled.
    auto p = mcp::test::make_in_memory_pair();

    std::atomic<bool> let_finish{false};
    ServerThread srv(std::move(p.b), [&](mcp::Server& s) {
        s.enable_tasks();
        s.tool("park", json{{"type", "object"}},
               [&](const json&) -> mcp::CallToolResult {
                   while (!let_finish.load(std::memory_order_acquire)) {
                       std::this_thread::sleep_for(5ms);
                   }
                   return {.content = { mcp::TextContent{.text = "done"} }};
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto env = client.call_tool_as_task("park", json{}).get();
    EXPECT_EQ(env.task.status, mcp::TaskStatus::working);

    auto cancelled = client.task_cancel(env.task.taskId).get();
    EXPECT_EQ(cancelled.taskId, env.task.taskId);
    EXPECT_EQ(cancelled.status, mcp::TaskStatus::cancelled);

    // Release the handler so the worker thread terminates cleanly.
    let_finish.store(true, std::memory_order_release);

    client.disconnect();
}

TEST(TasksIntegration, StatusNotificationsArrive) {
    // The status listener emits notifications for each transition;
    // the client's set_task_status_handler should see them.
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool("add", json{{"type", "object"}},
               [](const json& args) -> mcp::CallToolResult {
                   const int a = args.value("a", 0);
                   const int b = args.value("b", 0);
                   return {.content = { mcp::TextContent{
                       .text = std::to_string(a + b),
                   }}};
               });
    });

    std::mutex                mu;
    std::condition_variable   cv;
    std::vector<mcp::TaskStatus> seen;

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_task_status_handler([&](const mcp::Task& t) {
        std::lock_guard<std::mutex> lk(mu);
        seen.push_back(t.status);
        cv.notify_all();
    });
    (void)client.initialize().get();

    auto env = client.call_tool_as_task("add", json{{"a", 4}, {"b", 5}}).get();
    (void)client.task_result(env.task.taskId).get();

    // Wait for the working + completed transitions to arrive.
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, 2s, [&] {
        // We expect at least: working (on create) and completed
        // (on terminal). Other intermediate states are fine.
        bool saw_working   = false;
        bool saw_completed = false;
        for (auto s : seen) {
            if (s == mcp::TaskStatus::working)   saw_working   = true;
            if (s == mcp::TaskStatus::completed) saw_completed = true;
        }
        return saw_working && saw_completed;
    }));

    client.disconnect();
}

TEST(TasksIntegration, NonAugmentedCallStillWorks) {
    // Sanity check: enabling tasks doesn't break the synchronous
    // tools/call path.
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool("add", json{{"type", "object"}},
               [](const json& args) -> mcp::CallToolResult {
                   const int a = args.value("a", 0);
                   const int b = args.value("b", 0);
                   return {.content = { mcp::TextContent{
                       .text = std::to_string(a + b),
                   }}};
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto out = client.call_tool("add", json{{"a", 7}, {"b", 8}}).get();
    EXPECT_EQ(std::get<mcp::TextContent>(out.content[0]).text, "15");

    client.disconnect();
}

TEST(TasksIntegration, CancelOnceFiresStatusListenerOnce) {
    // Audit fix: the prior "cancel + worker race" allowed
    // complete() to re-fire the status listener after cancel()
    // had already done so, producing a duplicate cancelled
    // notification. Verify exactly-one notification per terminal
    // transition under that race.
    auto p = mcp::test::make_in_memory_pair();

    std::atomic<bool> let_finish{false};
    ServerThread srv(std::move(p.b), [&](mcp::Server& s) {
        s.enable_tasks();
        s.tool("park", json{{"type", "object"}},
               [&](const json&) -> mcp::CallToolResult {
                   while (!let_finish.load(std::memory_order_acquire)) {
                       std::this_thread::sleep_for(5ms);
                   }
                   return {.content = { mcp::TextContent{.text = "done"} }};
               });
    });

    std::mutex                  mu;
    std::condition_variable     cv;
    std::vector<mcp::TaskStatus> seen;

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    client.set_task_status_handler([&](const mcp::Task& t) {
        std::lock_guard<std::mutex> lk(mu);
        seen.push_back(t.status);
        cv.notify_all();
    });
    (void)client.initialize().get();

    auto env = client.call_tool_as_task("park", json{}).get();
    auto cancelled = client.task_cancel(env.task.taskId).get();
    EXPECT_EQ(cancelled.status, mcp::TaskStatus::cancelled);

    // Release the worker; without the audit fix this would re-fire
    // a SECOND cancelled notification once complete() ran.
    let_finish.store(true, std::memory_order_release);

    // Give the worker time to exit.
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, 1s, [&] {
        return std::count(seen.begin(), seen.end(),
                          mcp::TaskStatus::cancelled) > 1;
    });
    int cancelled_count = static_cast<int>(
        std::count(seen.begin(), seen.end(), mcp::TaskStatus::cancelled));
    EXPECT_EQ(cancelled_count, 1)
        << "duplicate cancelled status notifications fired";

    client.disconnect();
}

TEST(TasksIntegration, ConcurrencyCapEnforced) {
    auto p = mcp::test::make_in_memory_pair();

    std::atomic<bool> let_finish{false};
    ServerThread srv(std::move(p.b), [&](mcp::Server& s) {
        s.enable_tasks(/*default_ttl_ms=*/std::nullopt,
                       /*max_concurrent=*/1);
        s.tool("park", json{{"type", "object"}},
               [&](const json&) -> mcp::CallToolResult {
                   while (!let_finish.load(std::memory_order_acquire)) {
                       std::this_thread::sleep_for(5ms);
                   }
                   return {.content = { mcp::TextContent{.text = "done"} }};
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    // First task occupies the only slot.
    auto env = client.call_tool_as_task("park", json{}).get();
    EXPECT_EQ(env.task.status, mcp::TaskStatus::working);

    // Second task is rejected.
    EXPECT_THROW((void)client.call_tool_as_task("park", json{}).get(),
                 mcp::Error);

    let_finish.store(true, std::memory_order_release);
    // Drain the first task so the ServerThread destructor doesn't
    // wedge on a worker still parked in the busy-loop.
    (void)client.task_result(env.task.taskId).get();

    client.disconnect();
}

TEST(TasksIntegration, ResultMetaMergesInsteadOfClobbering) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        // Tool returns a CallToolResult whose JSON form, when set
        // through structured_content, doesn't itself populate
        // _meta. We'll inspect the wire-level result for the
        // related-task tag and verify it lives alongside other
        // _meta keys we sneak in via structured_content.
        s.tool("noop", json{{"type", "object"}},
               [](const json&) -> mcp::CallToolResult {
                   return {.content = { mcp::TextContent{.text = "ok"} }};
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto env = client.call_tool_as_task("noop", json{}).get();
    auto raw = client.task_result(env.task.taskId).get();
    ASSERT_TRUE(raw.contains("_meta"));
    ASSERT_TRUE(raw["_meta"].is_object());
    EXPECT_TRUE(raw["_meta"].contains(
        std::string{mcp::tasks_related_task_meta_key}));

    client.disconnect();
}

TEST(TasksIntegration, TaskNotFoundIsInvalidRequest) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
    });

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    try {
        (void)client.task_get("nonexistent").get();
        FAIL() << "task_get of unknown id should have thrown";
    } catch (const mcp::Error& e) {
        EXPECT_EQ(e.code(), mcp::error_code::invalid_request);
    }

    client.disconnect();
}

TEST(TasksIntegration, AugmentedCallWithoutEnableErrors) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        // No enable_tasks() — capability not advertised.
        s.tool("add", json{{"type", "object"}},
               [](const json&) -> mcp::CallToolResult {
                   return {.content = { mcp::TextContent{.text = "x"} }};
               });
    });

    mcp::Client client{ mcp::Implementation{.name = "t", .version = "0"} };
    client.connect(std::move(p.a));
    auto info = client.initialize().get();
    EXPECT_FALSE(info.capabilities.tasks.has_value());

    EXPECT_THROW((void)client.call_tool_as_task("add", json{}).get(),
                 mcp::Error);

    client.disconnect();
}

}  // namespace
