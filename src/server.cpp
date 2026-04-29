// SPDX-License-Identifier: Apache-2.0
#include "mcp/server.hpp"

#include "mcp/error.hpp"
#include "mcp/log.hpp"
#include "mcp/protocol.hpp"
#include "mcp/session.hpp"
#include "mcp/transport.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp {

namespace detail {

// ISO 8601 UTC timestamp at millisecond granularity. The spec
// doesn't mandate ms precision, but two transitions inside the
// same wall-clock second otherwise produce identical
// `lastUpdatedAt` values, which makes it look to clients like
// no transition happened.
inline std::string iso_utc_now() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto t   = clock::to_time_t(now);
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    const int n = std::snprintf(buf, sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<long long>(ms));
    return std::string{buf, static_cast<std::size_t>(n)};
}

// 128-bit hex token. Per-call seeded — not cryptographic, but
// non-adversarial unguessability is sufficient: task ids only
// need to be unique within a single Server lifetime.
inline std::string mint_task_id() {
    std::random_device rd;
    std::mt19937_64 rng{(static_cast<std::uint64_t>(rd()) << 32) ^ rd()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream os;
    os << std::hex << dist(rng) << dist(rng);
    return os.str();
}

// =====================================================================
// TaskStore
//
// Bookkeeping for all in-flight tasks owned by a Server. The store
// is thread-safe; callers can create / get / list / cancel from any
// thread. Per-task state transitions fire a single store-wide cv
// (`cv_`) so wait_result() readers wake on every change without us
// having to maintain a per-entry condvar.
// =====================================================================
class TaskStore {
public:
    using StatusListener = std::function<void(const Task&)>;

    /// Create a new task in `working` state. Returns the task envelope
    /// the requester sees in CreateTaskResult.
    Task create(std::optional<std::int64_t> ttl_ms);

    /// Mark a task as completed and stash its result payload. Wakes
    /// any tasks/result waiter and notifies the StatusListener.
    void complete(const std::string& id, nlohmann::json result);

    /// Mark a task as failed with the given message.
    void fail(const std::string& id, std::string message);

    /// Cooperative cancel: marks the task cancelled and wakes
    /// waiters. Returns the cancelled-projection of the task, or
    /// std::nullopt if no such task. Per spec, cancelling an already
    /// terminal task is a no-op (we just return its current state).
    std::optional<Task> cancel(const std::string& id);

    /// Snapshot the task envelope by id.
    [[nodiscard]] std::optional<Task> get(const std::string& id) const;

    /// Block until the named task hits a terminal status, or `timeout`
    /// elapses. Returns the final result payload on completed, throws
    /// Error on failed/cancelled, or returns std::nullopt if the task
    /// wasn't terminal in time.
    std::optional<nlohmann::json>
    wait_result(const std::string& id, std::chrono::milliseconds timeout);

    /// Paginated listing in creation order.
    [[nodiscard]] ListTasksResult
    list(std::optional<std::string> cursor, std::size_t page_size) const;

    /// Read the cooperative cancellation flag for a task. Returns
    /// false if the task isn't known.
    [[nodiscard]] bool is_cancelled(const std::string& id) const;

    /// Set once at startup; called on every task transition with a
    /// projection of the current task.
    void set_status_listener(StatusListener l);

    /// Sweep tasks whose ttl has elapsed since creation. Called by
    /// the background gc thread. Returns the count of removed tasks.
    std::size_t gc_expired();

    /// Track a detached worker thread. Increment on spawn, decrement
    /// when the worker is done. shutdown() blocks until the count
    /// reaches zero so the Server destructor can safely destroy the
    /// store without leaving dangling references in still-running
    /// workers.
    void worker_started() noexcept;
    void worker_finished() noexcept;

    /// Snapshot of the current in-flight worker count. Used by
    /// the task-augmented call path to enforce a concurrency cap.
    [[nodiscard]] int workers_inflight() const noexcept;

    /// Mark all in-flight tasks failed and wake their waiters, then
    /// block until any in-flight worker threads have returned. Called
    /// at Server teardown.
    void shutdown();

private:
    struct Entry {
        Task                                      task;
        std::optional<nlohmann::json>             result;
        std::optional<std::string>                error;
        std::atomic<bool>                         cancelled{false};
        std::chrono::steady_clock::time_point     created_steady;
    };

    [[nodiscard]] static bool is_terminal(TaskStatus s) noexcept {
        return s == TaskStatus::completed ||
               s == TaskStatus::failed    ||
               s == TaskStatus::cancelled;
    }

    void touch_locked(Entry& e, TaskStatus new_status,
                      std::optional<std::string> message) {
        e.task.status          = new_status;
        e.task.last_updated_at = iso_utc_now();
        if (message.has_value()) e.task.status_message = std::move(*message);
    }

    mutable std::mutex                                         mu_;
    std::condition_variable                                    cv_;
    std::unordered_map<std::string, std::unique_ptr<Entry>>    entries_;
    std::vector<std::string>                                   order_;
    StatusListener                                             listener_;
    std::atomic<bool>                                          shutting_down_{false};

    // In-flight worker bookkeeping. workers_mu_ + workers_cv_ are
    // held only briefly around increments/decrements; shutdown()
    // takes the lock and waits.
    mutable std::mutex                                         workers_mu_;
    std::condition_variable                                    workers_cv_;
    int                                                        workers_inflight_{0};
};

Task TaskStore::create(std::optional<std::int64_t> ttl_ms) {
    auto entry = std::make_unique<Entry>();
    entry->task.taskId          = mint_task_id();
    entry->task.status          = TaskStatus::working;
    entry->task.created_at      = iso_utc_now();
    entry->task.last_updated_at = entry->task.created_at;
    entry->task.ttl             = ttl_ms;
    // Suggest 500 ms polling. The spec lets receivers advertise a
    // hint; fixed-rate polling is fine for in-process tasks.
    entry->task.poll_interval   = 500;
    entry->created_steady       = std::chrono::steady_clock::now();

    Task projection;
    StatusListener listener_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        projection = entry->task;
        order_.push_back(entry->task.taskId);
        entries_.emplace(entry->task.taskId, std::move(entry));
        listener_copy = listener_;
    }
    if (listener_copy) listener_copy(projection);
    return projection;
}

void TaskStore::complete(const std::string& id, nlohmann::json result) {
    Task projection;
    StatusListener listener_copy;
    bool fire_notify = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(id);
        if (it == entries_.end()) return;
        auto& e = *it->second;
        if (is_terminal(e.task.status)) {
            // Already terminal — typical case is `cancel()` got
            // there first. Drop the result silently and DO NOT
            // re-notify or re-fire the listener; the spec says
            // terminal states have no further transitions, and
            // firing twice would emit a duplicate
            // notifications/tasks/status to the client.
            return;
        }
        if (e.cancelled.load(std::memory_order_acquire)) {
            // Cancellation race: cancel() set the flag before we
            // could; treat as if the worker observed cancellation.
            // (We still update the timestamp / status here because
            // the task wasn't yet in a terminal state.)
            touch_locked(e, TaskStatus::cancelled, std::string{"cancelled"});
        } else {
            e.result = std::move(result);
            touch_locked(e, TaskStatus::completed, std::nullopt);
        }
        projection    = e.task;
        listener_copy = listener_;
        fire_notify   = true;
    }
    if (fire_notify) {
        cv_.notify_all();
        if (listener_copy) listener_copy(projection);
    }
}

void TaskStore::fail(const std::string& id, std::string message) {
    Task projection;
    StatusListener listener_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(id);
        if (it == entries_.end()) return;
        auto& e = *it->second;
        if (is_terminal(e.task.status)) return;
        e.error = message;
        touch_locked(e, TaskStatus::failed, message);
        projection    = e.task;
        listener_copy = listener_;
    }
    cv_.notify_all();
    if (listener_copy) listener_copy(projection);
}

std::optional<Task> TaskStore::cancel(const std::string& id) {
    Task projection;
    StatusListener listener_copy;
    bool fire_listener = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(id);
        if (it == entries_.end()) return std::nullopt;
        auto& e = *it->second;
        // Don't touch the cancelled flag for an already-terminal
        // task. The flag exists so an in-flight worker's complete()
        // can take the cancellation-wins branch; once the task is
        // terminal there's no worker still racing, and flipping the
        // flag is purely confusing for any future invariant check.
        if (is_terminal(e.task.status)) {
            projection    = e.task;
            listener_copy = listener_;
        } else {
            e.cancelled.store(true, std::memory_order_release);
            touch_locked(e, TaskStatus::cancelled, std::string{"cancelled"});
            projection    = e.task;
            listener_copy = listener_;
            fire_listener = true;
        }
    }
    if (fire_listener) {
        cv_.notify_all();
        if (listener_copy) listener_copy(projection);
    }
    return projection;
}

std::optional<Task> TaskStore::get(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end()) return std::nullopt;
    return it->second->task;
}

bool TaskStore::is_cancelled(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    return it->second->cancelled.load(std::memory_order_acquire);
}

std::optional<nlohmann::json>
TaskStore::wait_result(const std::string& id,
                       std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!cv_.wait_for(lk, timeout, [&] {
            auto it = entries_.find(id);
            if (it == entries_.end()) return true;  // gone — also "terminal"
            return is_terminal(it->second->task.status) ||
                   shutting_down_.load(std::memory_order_acquire);
        })) {
        // Timed out without reaching terminal.
        return std::nullopt;
    }
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        // The id is well-formed; it just doesn't refer to a known
        // task. invalid_request is the closer error class than
        // invalid_params (the params shape was fine).
        throw Error{error_code::invalid_request,
                    "task not found: " + id};
    }
    auto& e = *it->second;
    switch (e.task.status) {
        case TaskStatus::completed:
            if (e.result.has_value()) return *e.result;
            // Shouldn't happen, but be defensive.
            throw Error{error_code::internal_error,
                        "task completed without a result payload"};
        case TaskStatus::failed:
            throw Error{error_code::internal_error,
                        "task failed: " + e.error.value_or(std::string{"<no message>"})};
        case TaskStatus::cancelled:
            throw Error{error_code::invalid_request,
                        "task cancelled: " + id};
        default:
            // Reached on shutdown if not terminal. Treat as failure.
            throw Error{error_code::internal_error,
                        "server shutting down before task completed"};
    }
}

ListTasksResult
TaskStore::list(std::optional<std::string> cursor,
                std::size_t                page_size) const {
    std::lock_guard<std::mutex> lk(mu_);

    std::size_t offset = 0;
    if (cursor.has_value() && !cursor->empty()) {
        // Same strict cursor format as paginate(): decimal digits only.
        const auto& s = *cursor;
        for (char c : s) {
            if (c < '0' || c > '9') {
                throw Error{error_code::invalid_params,
                            "invalid pagination cursor: " + s};
            }
        }
        try { offset = static_cast<std::size_t>(std::stoull(s)); }
        catch (...) { throw Error{error_code::invalid_params,
                                  "invalid pagination cursor: " + s}; }
    }

    ListTasksResult out;
    if (offset >= order_.size()) return out;

    const std::size_t end = (page_size == 0)
        ? order_.size()
        : std::min(order_.size(), offset + page_size);
    out.tasks.reserve(end - offset);
    for (std::size_t i = offset; i < end; ++i) {
        auto it = entries_.find(order_[i]);
        if (it != entries_.end()) out.tasks.push_back(it->second->task);
    }
    if (end < order_.size()) out.next_cursor = std::to_string(end);
    return out;
}

void TaskStore::set_status_listener(StatusListener l) {
    std::lock_guard<std::mutex> lk(mu_);
    listener_ = std::move(l);
}

std::size_t TaskStore::gc_expired() {
    const auto now = std::chrono::steady_clock::now();
    std::size_t removed = 0;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.begin();
    while (it != entries_.end()) {
        const auto& e = *it->second;
        if (is_terminal(e.task.status) && e.task.ttl.has_value()) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - e.created_steady).count();
            if (age >= *e.task.ttl) {
                // Drop from order_ too. Linear, but ttl-driven gc is
                // expected to be rare.
                const auto id = it->first;
                order_.erase(std::remove(order_.begin(), order_.end(), id),
                             order_.end());
                it = entries_.erase(it);
                ++removed;
                continue;
            }
        }
        ++it;
    }
    return removed;
}

void TaskStore::worker_started() noexcept {
    std::lock_guard<std::mutex> lk(workers_mu_);
    ++workers_inflight_;
}

int TaskStore::workers_inflight() const noexcept {
    std::lock_guard<std::mutex> lk(workers_mu_);
    return workers_inflight_;
}

void TaskStore::worker_finished() noexcept {
    // Notify under the lock. If we dropped the lock first, shutdown()
    // could see workers_inflight_ == 0, return, and ~TaskStore could
    // destroy workers_cv_ while we were still inside notify_all() —
    // a TSan-visible race against pthread_cond_destroy.
    std::lock_guard<std::mutex> lk(workers_mu_);
    if (--workers_inflight_ == 0) {
        workers_cv_.notify_all();
    }
}

void TaskStore::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        shutting_down_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    // Block until every detached worker has returned. Without this
    // the Server destructor can race with a still-running task
    // worker that holds `this` and would call back into a freed
    // store.
    std::unique_lock<std::mutex> lk(workers_mu_);
    workers_cv_.wait(lk, [&] { return workers_inflight_ == 0; });
}

}  // namespace detail

Server::Server(Implementation server_info)
    : server_info_(std::move(server_info)) {}

Server::~Server() {
    // Defensive: if the user destroys the Server without first
    // stop()+joining the run() thread, at least notify the run()
    // wait so the thread can exit promptly. We still rely on the
    // user to join before destruction (the class is non-movable
    // and non-copyable; tools/handlers reference `*this`), but a
    // dangling ~Server() with the run thread still active is
    // undefined behaviour, and skipping the notify would just
    // wedge the user's program rather than fail loudly.
    stop_requested_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(stop_mu_);
        stop_cv_.notify_all();
    }
    // Drain detached task workers before tasks_ is destroyed —
    // detached worker threads hold raw pointers to tasks_.
    if (tasks_) tasks_->shutdown();
}

Server& Server::set_instructions(std::string s) {
    instructions_ = std::move(s);
    return *this;
}

Server& Server::set_page_size(std::size_t n) {
    page_size_ = n;
    return *this;
}

namespace {
// Decode an offset cursor. Empty / missing cursor means start at 0.
// Throws Error(invalid_params) on malformed cursors.
//
// The decode is strict: only ASCII digits, no sign character, no
// leading/trailing whitespace, no overflow. std::stoull accepts "-1"
// (silently wrapping to UINT64_MAX) and trailing junk like "1abc",
// neither of which we want to treat as a valid offset.
std::size_t decode_cursor(const std::optional<std::string>& cursor) {
    if (!cursor.has_value() || cursor->empty()) return 0;
    const auto& s = *cursor;
    if (s.size() > 20) {  // longer than UINT64_MAX's decimal length
        throw Error{error_code::invalid_params,
                    "invalid pagination cursor: " + s};
    }
    std::uint64_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            throw Error{error_code::invalid_params,
                        "invalid pagination cursor: " + s};
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            throw Error{error_code::invalid_params,
                        "invalid pagination cursor: " + s};
        }
        value = value * 10 + digit;
    }
    return static_cast<std::size_t>(value);
}

// Slice [items_begin, items_end) into a page. If page_size is 0 or
// items.size() <= offset+page_size, returns the remainder with no
// nextCursor; otherwise returns a slice of size page_size and a
// `nextCursor` for the next call.
template <typename T>
std::pair<std::vector<T>, std::optional<std::string>>
paginate(const std::vector<T>& items, std::size_t offset, std::size_t page_size) {
    if (offset >= items.size()) return {{}, std::nullopt};
    if (page_size == 0 || offset + page_size >= items.size()) {
        return {{items.begin() + static_cast<std::ptrdiff_t>(offset), items.end()},
                std::nullopt};
    }
    const std::size_t next = offset + page_size;
    return {{items.begin() + static_cast<std::ptrdiff_t>(offset),
             items.begin() + static_cast<std::ptrdiff_t>(next)},
            std::to_string(next)};
}
}  // namespace

Server& Server::tool(std::string                    name,
                     nlohmann::json                 input_schema,
                     ToolHandler                    handler,
                     std::optional<std::string>     title,
                     std::optional<std::string>     description,
                     std::optional<ToolAnnotations> annotations,
                     std::optional<nlohmann::json>  output_schema) {
    Tool t{
        .name          = name,
        .title         = std::move(title),
        .description   = std::move(description),
        .input_schema  = std::move(input_schema),
        .output_schema = std::move(output_schema),
        .annotations   = std::move(annotations),
        .icons         = std::nullopt,
        .execution     = std::nullopt,
        .meta          = std::nullopt,
    };
    std::lock_guard<std::mutex> lk(tools_mu_);
    tools_[name] = ToolEntry{std::move(t), std::move(handler)};
    return *this;
}

Server& Server::tool(std::string    name,
                     nlohmann::json input_schema,
                     ToolHandler    handler,
                     ToolMetadata   meta) {
    Tool t{
        .name          = name,
        .title         = std::nullopt,
        .description   = std::nullopt,
        .input_schema  = std::move(input_schema),
        .output_schema = std::nullopt,
        .annotations   = std::nullopt,
        .icons         = std::move(meta.icons),
        .execution     = std::move(meta.execution),
        .meta          = std::move(meta.meta),
    };
    std::lock_guard<std::mutex> lk(tools_mu_);
    tools_[name] = ToolEntry{std::move(t), std::move(handler)};
    return *this;
}

Server& Server::resource(Resource descriptor, ResourceReadHandler handler) {
    std::lock_guard<std::mutex> lk(resources_mu_);
    const std::string uri = descriptor.uri;
    resources_[uri] = ResourceEntry{std::move(descriptor), std::move(handler)};
    return *this;
}

Server& Server::resource_template(ResourceTemplate descriptor) {
    std::lock_guard<std::mutex> lk(resources_mu_);
    resource_templates_.push_back(std::move(descriptor));
    return *this;
}

Server& Server::fallback_resource_handler(ResourceReadHandler handler) {
    std::lock_guard<std::mutex> lk(resources_mu_);
    fallback_resource_handler_ = std::move(handler);
    return *this;
}

Server& Server::prompt(Prompt descriptor, PromptGetHandler handler) {
    std::lock_guard<std::mutex> lk(prompts_mu_);
    const std::string name = descriptor.name;
    prompts_[name] = PromptEntry{std::move(descriptor), std::move(handler)};
    return *this;
}

Server& Server::enable_logging(LoggingLevel initial_level) {
    logging_enabled_.store(true, std::memory_order_release);
    log_level_.store(initial_level, std::memory_order_release);
    return *this;
}

bool Server::log(LoggingLevel               level,
                 nlohmann::json             data,
                 std::optional<std::string> logger) {
    if (!logging_enabled_.load(std::memory_order_acquire)) return false;
    if (static_cast<int>(level) <
        static_cast<int>(log_level_.load(std::memory_order_acquire))) return false;
    auto session = acquire_session();
    if (!session) return false;
    LoggingMessageNotificationParams params{
        .level  = level,
        .logger = std::move(logger),
        .data   = std::move(data),
    };
    auto ec = session->send_notification(
        std::string{method_notifications_message},
        nlohmann::json(params));
    return !ec;
}

void Server::report_progress(const ProgressToken&        token,
                             double                      progress,
                             std::optional<double>       total,
                             std::optional<std::string>  message) {
    auto session = acquire_session();
    if (!session) return;
    ProgressNotificationParams params{
        .progress_token = token,
        .progress       = progress,
        .total          = total,
        .message        = std::move(message),
    };
    (void)session->send_notification(
        std::string{method_notifications_progress},
        nlohmann::json(params));
}

std::future<CreateMessageResult>
Server::sample(CreateMessageRequestParams params) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error,
                    "Server::sample: server is not running"};
    }
    auto inner = session->send_request(
        std::string{method_sampling_create_message},
        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> CreateMessageResult {
            return inner.get().get<CreateMessageResult>();
        });
}

std::future<ListRootsResult> Server::list_roots() {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error,
                    "Server::list_roots: server is not running"};
    }
    auto inner = session->send_request(std::string{method_roots_list}, nullptr);
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListRootsResult {
            return inner.get().get<ListRootsResult>();
        });
}

std::future<ElicitResult>
Server::elicit(ElicitRequestParams params) {
    return elicit(std::move(params), std::chrono::milliseconds{0});
}

std::future<ElicitResult>
Server::elicit(ElicitRequestParams       params,
               std::chrono::milliseconds timeout) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error,
                    "Server::elicit: server is not running"};
    }
    // 0 ⇒ Session's default request timeout (30 s). For URL-mode
    // human-in-the-loop flows callers should pass a larger value.
    auto inner = session->send_request(
        std::string{method_elicitation_create},
        nlohmann::json(params),
        timeout);
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ElicitResult {
            return inner.get().get<ElicitResult>();
        });
}

void Server::set_elicitation_complete_handler(
    ElicitationCompleteHandler handler) {
    std::lock_guard<std::mutex> lk(completion_mu_);
    elicitation_complete_handler_ = std::move(handler);
}

std::error_code
Server::notify_elicitation_complete(std::string elicitation_id) {
    auto session = acquire_session();
    if (!session) return std::make_error_code(std::errc::not_connected);
    return session->send_notification(
        std::string{method_notifications_elicitation_complete},
        nlohmann::json(ElicitationCompleteNotificationParams{
            .elicitation_id = std::move(elicitation_id),
        }));
}

std::shared_ptr<Session> Server::acquire_session() const {
    std::lock_guard<std::mutex> lk(session_mu_);
    return session_;
}

Server& Server::enable_completion(CompletionHandler handler) {
    std::lock_guard<std::mutex> lk(completion_mu_);
    completion_handler_ = std::move(handler);
    return *this;
}

Server& Server::enable_tasks(std::optional<std::int64_t> default_ttl_ms,
                              std::size_t                 max_concurrent) {
    if (!tasks_) tasks_ = std::make_unique<detail::TaskStore>();
    tasks_default_ttl_ms_ = default_ttl_ms;
    tasks_max_concurrent_ = max_concurrent;
    return *this;
}

// =====================================================================
// Handlers
// =====================================================================

nlohmann::json Server::handle_initialize(const nlohmann::json& params) {
    InitializeRequestParams parsed;
    try {
        parsed = params.get<InitializeRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid initialize params: "} + e.what()};
    }

    // Atomically claim the "initialized" slot — only the first concurrent
    // initialize call may pass through, even if multiple arrived on
    // different worker threads.
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
        throw Error{error_code::invalid_request, "already initialized"};
    }

    // Per spec: if we don't support the client's protocol version, we
    // respond with the version we *do* support; the client may then
    // disconnect. For Phase 1 we simply echo our own latest.
    InitializeResult result{};
    result.protocol_version = std::string{kLatestProtocolVersion};
    result.server_info      = server_info_;
    result.instructions     = instructions_;

    // Capabilities: we offer each capability iff something is registered
    // for it. listChanged is unset because we don't yet emit those
    // notifications (Phase 3).
    {
        std::lock_guard<std::mutex> lk(tools_mu_);
        if (!tools_.empty()) result.capabilities.tools = ToolsCapability{};
    }
    {
        std::lock_guard<std::mutex> lk(resources_mu_);
        if (!resources_.empty() || !resource_templates_.empty() ||
            fallback_resource_handler_) {
            result.capabilities.resources = ResourcesCapability{};
        }
    }
    {
        std::lock_guard<std::mutex> lk(prompts_mu_);
        if (!prompts_.empty()) result.capabilities.prompts = PromptsCapability{};
    }
    if (logging_enabled_.load(std::memory_order_acquire)) {
        result.capabilities.logging = nlohmann::json::object();
    }
    {
        std::lock_guard<std::mutex> lk(completion_mu_);
        if (completion_handler_) {
            result.capabilities.completions = nlohmann::json::object();
        }
    }
    if (tasks_) {
        TasksCapability tc;
        tc.list   = nlohmann::json::object();
        tc.cancel = nlohmann::json::object();
        TasksRequestsCapability rc;
        rc.tools  = nlohmann::json{{"call", nlohmann::json::object()}};
        tc.requests = std::move(rc);
        result.capabilities.tasks = std::move(tc);
    }

    return result;
}

nlohmann::json Server::handle_list_tools(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    auto parsed = params.is_null()
                      ? ListToolsRequestParams{}
                      : params.get<ListToolsRequestParams>();
    std::vector<Tool> all;
    {
        std::lock_guard<std::mutex> lk(tools_mu_);
        all.reserve(tools_.size());
        for (const auto& [name, entry] : tools_) all.push_back(entry.descriptor);
    }
    auto [page, next] = paginate(all, decode_cursor(parsed.cursor), page_size_);
    return ListToolsResult{
        .tools       = std::move(page),
        .next_cursor = std::move(next),
        .meta        = std::nullopt,
    };
}

nlohmann::json Server::handle_call_tool(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    CallToolRequestParams parsed;
    try {
        parsed = params.get<CallToolRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid tools/call params: "} + e.what()};
    }

    ToolHandler h;
    std::optional<TaskSupport> tool_task_support;
    {
        std::lock_guard<std::mutex> lk(tools_mu_);
        auto it = tools_.find(parsed.name);
        if (it == tools_.end()) {
            throw Error{error_code::method_not_found,
                        "tool not found: " + parsed.name};
        }
        h = it->second.handler;
        if (it->second.descriptor.execution.has_value()) {
            tool_task_support = it->second.descriptor.execution->task_support;
        }
    }
    const auto args = parsed.arguments.value_or(nlohmann::json::object());

    // 2025-11-25: per-tool execution policy gates task augmentation.
    // When the tool declares `execution.taskSupport == "required"`, a
    // synchronous (non-augmented) call is rejected. When it declares
    // `forbidden`, an augmented call is rejected. Default ("optional"
    // / absent) accepts either path.
    if (tool_task_support.has_value()) {
        if (*tool_task_support == TaskSupport::required && !parsed.task.has_value()) {
            throw Error{error_code::invalid_request,
                        "tool \"" + parsed.name +
                        "\" requires task augmentation; pass `task` in params"};
        }
        if (*tool_task_support == TaskSupport::forbidden && parsed.task.has_value()) {
            throw Error{error_code::invalid_request,
                        "tool \"" + parsed.name +
                        "\" forbids task augmentation"};
        }
    }

    if (parsed.task.has_value()) {
        if (!tasks_) {
            throw Error{error_code::invalid_request,
                        "tasks/tools/call: server has not enabled the tasks "
                        "capability"};
        }
        // DoS hardening: reject if we'd exceed the configured
        // concurrent-task cap. 0 means unlimited (default).
        if (tasks_max_concurrent_ > 0 &&
            static_cast<std::size_t>(tasks_->workers_inflight())
                >= tasks_max_concurrent_) {
            throw Error{error_code::invalid_request,
                        "task concurrency limit reached (" +
                        std::to_string(tasks_max_concurrent_) +
                        ")"};
        }
        // Resolve effective TTL: per-call override wins, else the
        // server-wide default.
        const auto ttl = parsed.task->ttl.has_value()
                             ? parsed.task->ttl
                             : tasks_default_ttl_ms_;
        const Task task = tasks_->create(ttl);

        // Dispatch the actual tool call onto a worker thread. The
        // request handler is itself running on a Session worker (the
        // Session detaches inbound requests so server-side handlers
        // can issue further nested requests without deadlock), but
        // we still want the *task* worker decoupled so this handler
        // returns its CreateTaskResult immediately.
        //
        // worker_started/finished bracket the worker so TaskStore
        // shutdown() can wait for it to exit before the Server is
        // destroyed.
        const std::string id = task.taskId;
        tasks_->worker_started();
        std::thread([store = tasks_.get(), h = std::move(h),
                      args = std::move(args), id]() mutable {
            try {
                CallToolResult res = h(args);
                store->complete(id, nlohmann::json(res));
            } catch (const Error& e) {
                store->fail(id, e.message());
            } catch (const std::exception& e) {
                store->fail(id, e.what());
            } catch (...) {
                store->fail(id, "unknown exception");
            }
            store->worker_finished();
        }).detach();

        return CreateTaskResult{.task = task};
    }

    return h(args);
}

nlohmann::json Server::handle_list_resources(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    auto parsed = params.is_null()
                      ? ListResourcesRequestParams{}
                      : params.get<ListResourcesRequestParams>();
    std::vector<Resource> all;
    {
        std::lock_guard<std::mutex> lk(resources_mu_);
        all.reserve(resources_.size());
        for (const auto& [uri, entry] : resources_) all.push_back(entry.descriptor);
    }
    auto [page, next] = paginate(all, decode_cursor(parsed.cursor), page_size_);
    return ListResourcesResult{
        .resources   = std::move(page),
        .next_cursor = std::move(next),
        .meta        = std::nullopt,
    };
}

nlohmann::json Server::handle_list_resource_templates(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    auto parsed = params.is_null()
                      ? ListResourceTemplatesRequestParams{}
                      : params.get<ListResourceTemplatesRequestParams>();
    std::vector<ResourceTemplate> all;
    {
        std::lock_guard<std::mutex> lk(resources_mu_);
        all = resource_templates_;
    }
    auto [page, next] = paginate(all, decode_cursor(parsed.cursor), page_size_);
    return ListResourceTemplatesResult{
        .resource_templates = std::move(page),
        .next_cursor        = std::move(next),
        .meta               = std::nullopt,
    };
}

nlohmann::json Server::handle_read_resource(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    ReadResourceRequestParams parsed;
    try {
        parsed = params.get<ReadResourceRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid resources/read params: "} + e.what()};
    }

    ResourceReadHandler h;
    {
        std::lock_guard<std::mutex> lk(resources_mu_);
        auto it = resources_.find(parsed.uri);
        if (it != resources_.end()) {
            h = it->second.handler;
        } else {
            h = fallback_resource_handler_;
        }
    }
    if (!h) {
        throw Error{error_code::method_not_found,
                    "resource not found: " + parsed.uri};
    }
    return h(parsed.uri);
}

nlohmann::json Server::handle_list_prompts(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    auto parsed = params.is_null()
                      ? ListPromptsRequestParams{}
                      : params.get<ListPromptsRequestParams>();
    std::vector<Prompt> all;
    {
        std::lock_guard<std::mutex> lk(prompts_mu_);
        all.reserve(prompts_.size());
        for (const auto& [name, entry] : prompts_) all.push_back(entry.descriptor);
    }
    auto [page, next] = paginate(all, decode_cursor(parsed.cursor), page_size_);
    return ListPromptsResult{
        .prompts     = std::move(page),
        .next_cursor = std::move(next),
        .meta        = std::nullopt,
    };
}

nlohmann::json Server::handle_get_prompt(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    GetPromptRequestParams parsed;
    try {
        parsed = params.get<GetPromptRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid prompts/get params: "} + e.what()};
    }

    PromptGetHandler h;
    {
        std::lock_guard<std::mutex> lk(prompts_mu_);
        auto it = prompts_.find(parsed.name);
        if (it != prompts_.end()) h = it->second.handler;
    }
    if (!h) {
        throw Error{error_code::method_not_found,
                    "prompt not found: " + parsed.name};
    }
    const auto args = parsed.arguments.value_or(
        std::unordered_map<std::string, std::string>{});
    return h(args);
}

nlohmann::json Server::handle_ping(const nlohmann::json& /*params*/) {
    return nlohmann::json::object();
}

nlohmann::json Server::handle_set_level(const nlohmann::json& params) {
    if (!logging_enabled_.load(std::memory_order_acquire)) {
        throw Error{error_code::method_not_found,
                    "logging capability is not enabled"};
    }
    SetLevelRequestParams parsed;
    try {
        parsed = params.get<SetLevelRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid logging/setLevel: "} + e.what()};
    }
    log_level_.store(parsed.level, std::memory_order_release);
    return nlohmann::json::object();
}

nlohmann::json Server::handle_complete(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    CompletionHandler h;
    {
        std::lock_guard<std::mutex> lk(completion_mu_);
        h = completion_handler_;
    }
    if (!h) {
        throw Error{error_code::method_not_found,
                    "completion is not enabled on this server"};
    }
    CompleteRequestParams parsed;
    try {
        parsed = params.get<CompleteRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid completion/complete params: "} + e.what()};
    }
    return CompleteResult{.completion = h(parsed)};
}

nlohmann::json Server::handle_get_task(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    if (!tasks_) {
        throw Error{error_code::method_not_found,
                    "tasks: server has not enabled the tasks capability"};
    }
    GetTaskRequestParams parsed;
    try {
        parsed = params.get<GetTaskRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid tasks/get params: "} + e.what()};
    }
    auto t = tasks_->get(parsed.task_id);
    if (!t.has_value()) {
        throw Error{error_code::invalid_request,
                    "task not found: " + parsed.task_id};
    }
    return *t;
}

nlohmann::json Server::handle_get_task_result(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    if (!tasks_) {
        throw Error{error_code::method_not_found,
                    "tasks: server has not enabled the tasks capability"};
    }
    GetTaskResultRequestParams parsed;
    try {
        parsed = params.get<GetTaskResultRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid tasks/result params: "} + e.what()};
    }
    // Block up to a hair under the JSON-RPC default request timeout
    // so the requester gets a clean error rather than a timeout if
    // the work overruns. The spec lets clients re-poll as needed.
    constexpr auto wait_budget = std::chrono::milliseconds{25'000};
    auto value = tasks_->wait_result(parsed.task_id, wait_budget);
    if (!value.has_value()) {
        // The task is still running and the long-poll budget expired.
        // We can't return a partial result on the wire (tasks/result's
        // shape is the underlying request's result), so surface a
        // structured error and let the caller re-poll. The error
        // payload echoes the current Task projection so the client
        // doesn't have to chase down `tasks/get` to see status.
        nlohmann::json data = nlohmann::json::object();
        if (auto cur = tasks_->get(parsed.task_id); cur.has_value()) {
            data["task"] = *cur;
        }
        throw Error{error_code::invalid_request,
                    "task " + parsed.task_id +
                    " not yet terminal; re-call tasks/result or "
                    "poll tasks/get",
                    std::move(data)};
    }
    // Tag the result with the spec's mandated _meta key so callers
    // can correlate. Merge into any existing _meta the handler set
    // (don't clobber); tolerate a non-object _meta by replacing it
    // with one (loud preservation of arbitrary prior shapes is
    // worse than the small loss of a wrongly-typed _meta value).
    if (value->is_object()) {
        auto& meta = (*value)["_meta"];
        if (!meta.is_object()) meta = nlohmann::json::object();
        meta[std::string{tasks_related_task_meta_key}] =
            nlohmann::json{{"taskId", parsed.task_id}};
    }
    return *value;
}

nlohmann::json Server::handle_list_tasks(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    if (!tasks_) {
        throw Error{error_code::method_not_found,
                    "tasks: server has not enabled the tasks capability"};
    }
    ListTasksRequestParams parsed;
    if (!params.is_null()) {
        try {
            parsed = params.get<ListTasksRequestParams>();
        } catch (const std::exception& e) {
            throw Error{error_code::invalid_params,
                        std::string{"invalid tasks/list params: "} + e.what()};
        }
    }
    return tasks_->list(parsed.cursor, page_size_);
}

nlohmann::json Server::handle_cancel_task(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    if (!tasks_) {
        throw Error{error_code::method_not_found,
                    "tasks: server has not enabled the tasks capability"};
    }
    CancelTaskRequestParams parsed;
    try {
        parsed = params.get<CancelTaskRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid tasks/cancel params: "} + e.what()};
    }
    auto t = tasks_->cancel(parsed.task_id);
    if (!t.has_value()) {
        throw Error{error_code::invalid_request,
                    "task not found: " + parsed.task_id};
    }
    return *t;
}

void Server::handle_cancelled(const nlohmann::json& params) {
    // The Server cannot abort an in-flight handler in Phase 2; we log
    // the cancellation so users know it arrived. Phase 3 will plumb a
    // cancellation token through to ToolHandler etc.
    try {
        auto p = params.get<CancelledNotificationParams>();
        if (p.request_id.has_value()) {
            MCP_LOG_INFO("client cancelled request id=" + p.request_id->canonical()
                         + (p.reason ? (" reason=" + *p.reason) : ""));
        }
    } catch (...) {
        MCP_LOG_WARN("malformed notifications/cancelled payload");
    }
}

// =====================================================================
// Lifecycle
// =====================================================================

void Server::run(std::unique_ptr<Transport> transport) {
    if (!transport) {
        throw Error{error_code::internal_error, "Server::run: transport is null"};
    }
    initialized_.store(false, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);

    auto local = std::make_shared<Session>(std::move(transport));
    {
        std::lock_guard<std::mutex> lk(session_mu_);
        session_ = local;
    }

    local->set_request_handler(std::string{method_initialize},
        [this](const nlohmann::json& p) { return handle_initialize(p); });
    local->set_request_handler(std::string{method_tools_list},
        [this](const nlohmann::json& p) { return handle_list_tools(p); });
    local->set_request_handler(std::string{method_tools_call},
        [this](const nlohmann::json& p) { return handle_call_tool(p); });
    local->set_request_handler(std::string{method_resources_list},
        [this](const nlohmann::json& p) { return handle_list_resources(p); });
    local->set_request_handler(std::string{method_resources_templates_list},
        [this](const nlohmann::json& p) { return handle_list_resource_templates(p); });
    local->set_request_handler(std::string{method_resources_read},
        [this](const nlohmann::json& p) { return handle_read_resource(p); });
    local->set_request_handler(std::string{method_prompts_list},
        [this](const nlohmann::json& p) { return handle_list_prompts(p); });
    local->set_request_handler(std::string{method_prompts_get},
        [this](const nlohmann::json& p) { return handle_get_prompt(p); });
    local->set_request_handler(std::string{method_ping},
        [this](const nlohmann::json& p) { return handle_ping(p); });
    local->set_request_handler(std::string{method_logging_set_level},
        [this](const nlohmann::json& p) { return handle_set_level(p); });
    local->set_request_handler(std::string{method_completion_complete},
        [this](const nlohmann::json& p) { return handle_complete(p); });

    if (tasks_) {
        local->set_request_handler(std::string{method_tasks_get},
            [this](const nlohmann::json& p) { return handle_get_task(p); });
        local->set_request_handler(std::string{method_tasks_result},
            [this](const nlohmann::json& p) { return handle_get_task_result(p); });
        local->set_request_handler(std::string{method_tasks_list},
            [this](const nlohmann::json& p) { return handle_list_tasks(p); });
        local->set_request_handler(std::string{method_tasks_cancel},
            [this](const nlohmann::json& p) { return handle_cancel_task(p); });

        // Push status updates to the client over notifications/tasks/status.
        // The listener fires while TaskStore holds its mutex... no — actually
        // we already drop the lock before calling the listener (see
        // TaskStore::complete/fail/cancel/create), so this is safe.
        std::weak_ptr<Session> weak{local};
        tasks_->set_status_listener(
            [weak](const Task& t) {
                auto s = weak.lock();
                if (!s) return;
                (void)s->send_notification(
                    std::string{method_notifications_tasks_status},
                    nlohmann::json(TaskStatusNotificationParams{.task = t}));
            });
    }

    // notifications/elicitation/complete (URL-mode finalization). The
    // handler may be set/cleared at any time; we resolve it lazily
    // under completion_mu_ on each fire.
    local->set_notification_handler(
        std::string{method_notifications_elicitation_complete},
        [this](const nlohmann::json& params) {
            ElicitationCompleteHandler h;
            {
                std::lock_guard<std::mutex> lk(completion_mu_);
                h = elicitation_complete_handler_;
            }
            if (!h || !params.is_object()) return;
            ElicitationCompleteNotificationParams parsed;
            try {
                parsed = params.get<ElicitationCompleteNotificationParams>();
            } catch (const std::exception& e) {
                MCP_LOG_WARN(std::string{"malformed elicitation/complete: "}
                             + e.what());
                return;
            } catch (...) {
                MCP_LOG_WARN("malformed elicitation/complete");
                return;
            }
            try { h(std::move(parsed.elicitation_id)); }
            catch (const std::exception& e) {
                MCP_LOG_ERROR(std::string{"elicitation/complete handler threw: "}
                              + e.what());
            } catch (...) {
                MCP_LOG_ERROR("elicitation/complete handler threw a non-std exception");
            }
        });

    local->set_notification_handler(std::string{method_notifications_cancelled},
        [this](const nlohmann::json& p) { handle_cancelled(p); });

    local->set_notification_handler(std::string{method_notifications_initialized},
        [](const nlohmann::json&) {
            // Per the spec, this notification just confirms the client is
            // ready for normal operation. We simply log it.
            MCP_LOG_DEBUG("client sent notifications/initialized");
        });

    local->set_on_closed([this]() {
        std::lock_guard<std::mutex> lk(stop_mu_);
        stop_cv_.notify_all();
    });

    local->start();

    {
        std::unique_lock<std::mutex> lk(stop_mu_);
        stop_cv_.wait(lk, [this, &local] {
            return stop_requested_.load(std::memory_order_acquire) ||
                   !local->is_open();
        });
    }
    local->close();
    {
        std::lock_guard<std::mutex> lk(session_mu_);
        session_.reset();
    }
}

void Server::stop() {
    // Notify under the lock so a concurrent run() loop can't unblock,
    // return, and start tearing down stop_cv_ before this notify_all
    // observes it. (Same pattern Session uses for its workers_cv_.)
    std::lock_guard<std::mutex> lk(stop_mu_);
    stop_requested_.store(true, std::memory_order_release);
    stop_cv_.notify_all();
}

}  // namespace mcp
