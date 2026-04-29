// SPDX-License-Identifier: Apache-2.0
//
// mcp-notes-server — a SQLite + FTS5 scratchpad for Claude.
//
// What it does
// ------------
// Plug this into a Claude Desktop / Claude Code config and Claude
// gains persistent, full-text-searchable memory across sessions.
// "Save my Postgres connection string under db.prod." "Search my
// notes for postgres." "What did I tell you about the auth flow?"
//
// Storage is a single SQLite database (default
// `~/.local/state/mcp-notes/notes.db`, override with --db). FTS5
// powers `notes/search` with porter-stemming + unicode61 tokenisation.
// `notes/delete` uses MCP elicitation to prompt the human before
// destructive ops on a glob.
//
// Wire it into Claude Desktop
// ---------------------------
//   {
//     "mcpServers": {
//       "notes": {
//         "command": "/path/to/mcp_notes_server",
//         "args": ["--db", "/Users/you/.notes.db"]
//       }
//     }
//   }
//
// Then ask Claude: "remember that my preferred indent is two spaces."
//
// Architecture / threading
// ------------------------
// Tools run on Session worker threads (one per inbound request);
// SQLite is configured serialised so a single sqlite3* handle is
// safe across them. The handler-level pattern is "open once at
// startup, run statements with prepared statements, never close".
//
// Cancellation: tool handlers do not currently honour
// notifications/cancelled — a bulk batch_write that consumes too
// many notes could in principle be slow. We keep individual
// statements bounded so the worst case is a single file scan.

#include "mcp/mcp.hpp"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// =====================================================================
// Tiny SQLite RAII shell — just enough for this server.
// =====================================================================

struct SqliteError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Stmt {
public:
    Stmt() = default;
    Stmt(sqlite3* db, std::string_view sql) {
        if (sqlite3_prepare_v2(db, sql.data(),
                                static_cast<int>(sql.size()),
                                &raw_, nullptr) != SQLITE_OK) {
            throw SqliteError(std::string{"sqlite prepare failed: "}
                              + sqlite3_errmsg(db));
        }
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&& o) noexcept : raw_(o.raw_) { o.raw_ = nullptr; }
    Stmt& operator=(Stmt&& o) noexcept {
        if (this != &o) {
            if (raw_) sqlite3_finalize(raw_);
            raw_ = o.raw_; o.raw_ = nullptr;
        }
        return *this;
    }
    ~Stmt() { if (raw_) sqlite3_finalize(raw_); }
    sqlite3_stmt* get() const noexcept { return raw_; }
    sqlite3_stmt* operator->() const noexcept { return raw_; }

    // Bind helpers. 1-indexed per SQLite convention.
    void bind(int i, std::string_view s) {
        // SQLITE_TRANSIENT: SQLite copies the buffer, so we don't
        // have to keep `s` alive.
        if (sqlite3_bind_text(raw_, i, s.data(),
                               static_cast<int>(s.size()),
                               SQLITE_TRANSIENT) != SQLITE_OK) {
            throw SqliteError("sqlite bind failed");
        }
    }
    void bind(int i, std::int64_t v) {
        if (sqlite3_bind_int64(raw_, i, v) != SQLITE_OK) {
            throw SqliteError("sqlite bind failed");
        }
    }
    void bind_null(int i) {
        if (sqlite3_bind_null(raw_, i) != SQLITE_OK) {
            throw SqliteError("sqlite bind failed");
        }
    }

    int step() { return sqlite3_step(raw_); }

    // Column readers.
    std::string text(int col) const {
        const auto* p = sqlite3_column_text(raw_, col);
        const auto  n = sqlite3_column_bytes(raw_, col);
        return p ? std::string{reinterpret_cast<const char*>(p),
                                static_cast<std::size_t>(n)}
                 : std::string{};
    }
    std::int64_t i64(int col) const { return sqlite3_column_int64(raw_, col); }
    bool is_null(int col) const {
        return sqlite3_column_type(raw_, col) == SQLITE_NULL;
    }

private:
    sqlite3_stmt* raw_ = nullptr;
};

class Db {
public:
    explicit Db(const std::string& path, bool readonly) {
        const int flags = readonly
            ? (SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX)
            : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
               | SQLITE_OPEN_FULLMUTEX);
        if (sqlite3_open_v2(path.c_str(), &handle_, flags, nullptr) != SQLITE_OK) {
            const std::string msg = handle_
                ? sqlite3_errmsg(handle_)
                : "unknown sqlite open failure";
            if (handle_) sqlite3_close(handle_);
            handle_ = nullptr;
            throw SqliteError(std::string{"sqlite open failed: "} + msg);
        }
        // Sensible defaults: WAL for concurrent readers, foreign keys on.
        if (!readonly) {
            (void)exec("PRAGMA journal_mode = WAL");
            (void)exec("PRAGMA synchronous  = NORMAL");
            (void)exec("PRAGMA foreign_keys = ON");
        }
    }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    ~Db() { if (handle_) sqlite3_close(handle_); }

    sqlite3* handle() const noexcept { return handle_; }

    void exec(std::string_view sql) {
        char* err = nullptr;
        // sqlite3_exec doesn't take a length — but our statements
        // are well-formed string_views over null-terminated
        // literals, so .data() is safe here.
        if (sqlite3_exec(handle_, std::string{sql}.c_str(),
                          nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string msg = err ? err : "unknown error";
            if (err) sqlite3_free(err);
            throw SqliteError(std::string{"sqlite exec failed: "} + msg);
        }
    }

    Stmt prepare(std::string_view sql) { return Stmt{handle_, sql}; }

private:
    sqlite3* handle_ = nullptr;
};

// =====================================================================
// Schema bootstrap — DDL that's idempotent so re-opens are safe.
// =====================================================================

// Standard rowid (no WITHOUT ROWID) so the FTS5 sync triggers can
// use new.rowid / old.rowid directly. `key` is UNIQUE; we still
// look notes up by key in the application but the FTS5 contentless
// virtual table joins on rowid.
constexpr std::string_view kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS notes (
    rowid      INTEGER PRIMARY KEY,
    key        TEXT UNIQUE NOT NULL,
    value      TEXT NOT NULL,
    tags       TEXT,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS notes_updated_at
  ON notes (updated_at DESC);

CREATE VIRTUAL TABLE IF NOT EXISTS notes_fts USING fts5 (
    key, value, tags,
    tokenize = 'porter unicode61'
);

-- Triggers keep notes_fts in sync with notes.
CREATE TRIGGER IF NOT EXISTS notes_ai AFTER INSERT ON notes BEGIN
    INSERT INTO notes_fts(rowid, key, value, tags)
    VALUES (new.rowid, new.key, new.value, COALESCE(new.tags, ''));
END;
CREATE TRIGGER IF NOT EXISTS notes_au AFTER UPDATE ON notes BEGIN
    DELETE FROM notes_fts WHERE rowid = old.rowid;
    INSERT INTO notes_fts(rowid, key, value, tags)
    VALUES (new.rowid, new.key, new.value, COALESCE(new.tags, ''));
END;
CREATE TRIGGER IF NOT EXISTS notes_ad AFTER DELETE ON notes BEGIN
    DELETE FROM notes_fts WHERE rowid = old.rowid;
END;
)sql";

// =====================================================================
// Helpers
// =====================================================================

std::int64_t now_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string default_db_path() {
    // Per XDG Base Directory: state files live under
    // $XDG_STATE_HOME (default ~/.local/state).
    const char* state = std::getenv("XDG_STATE_HOME");
    fs::path base;
    if (state && *state) {
        base = state;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = fs::path{home} / ".local" / "state";
    } else {
        base = fs::current_path();
    }
    base /= "mcp-notes";
    std::error_code ec;
    fs::create_directories(base, ec);
    return (base / "notes.db").string();
}

mcp::CallToolResult ok_text(std::string s) {
    return mcp::CallToolResult{
        .content = { mcp::TextContent{.text = std::move(s)} },
    };
}
mcp::CallToolResult ok_json(json structured, std::string human_summary) {
    return mcp::CallToolResult{
        .content = { mcp::TextContent{.text = std::move(human_summary)} },
        .structured_content = std::move(structured),
    };
}
mcp::CallToolResult err_text(std::string s) {
    return mcp::CallToolResult{
        .content = { mcp::TextContent{.text = std::move(s)} },
        .is_error = true,
    };
}

// =====================================================================
// Tool handlers
// =====================================================================

class NotesService {
public:
    NotesService(std::shared_ptr<Db> db, bool readonly)
        : db_(std::move(db)), readonly_(readonly) {}

    mcp::CallToolResult write(const json& args) {
        if (readonly_) return err_text("notes/write: server is read-only");
        const std::string key   = args.value("key", std::string{});
        const std::string value = args.value("value", std::string{});
        const std::string tags  = args.value("tags", std::string{});
        if (key.empty()) return err_text("notes/write: `key` is required");
        if (key.size() > 256)
            return err_text("notes/write: key too long (>256 chars)");
        if (value.empty())
            return err_text("notes/write: `value` is required");

        const auto now = now_unix_seconds();
        auto stmt = db_->prepare(R"sql(
            INSERT INTO notes(key, value, tags, created_at, updated_at)
            VALUES (?1, ?2, ?3, ?4, ?4)
            ON CONFLICT(key) DO UPDATE SET
                value      = excluded.value,
                tags       = excluded.tags,
                updated_at = excluded.updated_at
        )sql");
        stmt.bind(1, key);
        stmt.bind(2, value);
        if (tags.empty()) stmt.bind_null(3); else stmt.bind(3, tags);
        stmt.bind(4, now);
        if (stmt.step() != SQLITE_DONE) {
            return err_text(std::string{"notes/write: "} +
                            sqlite3_errmsg(db_->handle()));
        }
        return ok_text("ok: stored under key \"" + key + "\"");
    }

    mcp::CallToolResult read(const json& args) {
        const std::string key = args.value("key", std::string{});
        if (key.empty()) return err_text("notes/read: `key` is required");
        auto stmt = db_->prepare(
            "SELECT value, tags, created_at, updated_at "
            "FROM notes WHERE key = ?1");
        stmt.bind(1, key);
        const int rc = stmt.step();
        if (rc == SQLITE_DONE) {
            return err_text("notes/read: no note with key \"" + key + "\"");
        }
        if (rc != SQLITE_ROW) {
            return err_text(std::string{"notes/read: "} +
                            sqlite3_errmsg(db_->handle()));
        }
        json structured = {
            {"key",        key},
            {"value",      stmt.text(0)},
            {"tags",       stmt.is_null(1) ? "" : stmt.text(1)},
            {"createdAt",  stmt.i64(2)},
            {"updatedAt",  stmt.i64(3)},
        };
        return ok_json(std::move(structured), stmt.text(0));
    }

    mcp::CallToolResult list(const json& args) {
        const std::string tag    = args.value("tag",    std::string{});
        const std::string prefix = args.value("prefix", std::string{});
        const auto        limit  =
            std::clamp<std::int64_t>(args.value("limit", 50), 1, 1'000);

        std::string sql =
            "SELECT key, tags, updated_at FROM notes WHERE 1=1";
        std::vector<std::string> binds;
        if (!prefix.empty()) {
            sql += " AND key LIKE ?";
            binds.push_back(prefix + "%");
        }
        if (!tag.empty()) {
            // tags column is a comma-separated list; find a token-
            // matching value via a normalised LIKE.
            sql += " AND (',' || tags || ',') LIKE ?";
            binds.push_back("%," + tag + ",%");
        }
        sql += " ORDER BY updated_at DESC LIMIT ?";

        auto stmt = db_->prepare(sql);
        int idx = 1;
        for (const auto& b : binds) stmt.bind(idx++, b);
        stmt.bind(idx, limit);

        json items = json::array();
        while (stmt.step() == SQLITE_ROW) {
            items.push_back({
                {"key",       stmt.text(0)},
                {"tags",      stmt.is_null(1) ? "" : stmt.text(1)},
                {"updatedAt", stmt.i64(2)},
            });
        }
        std::string summary = std::to_string(items.size())
                            + " note(s) listed";
        return ok_json(json{{"notes", std::move(items)}}, std::move(summary));
    }

    mcp::CallToolResult search(const json& args) {
        const std::string query = args.value("query", std::string{});
        const auto        limit =
            std::clamp<std::int64_t>(args.value("limit", 25), 1, 200);
        if (query.empty()) return err_text("notes/search: `query` is required");

        // FTS5 query syntax — we hand the user's query through as-is
        // but defensively quote it as a single phrase if it looks like
        // a literal that the syntax would reject (commonly happens
        // when the LLM passes `key:value` etc.). FTS5 phrase
        // delimiter: double quotes.
        std::string fts_query = query;
        const bool has_special =
            fts_query.find_first_of("\"-+*()") != std::string::npos;
        if (has_special) {
            // Escape internal double quotes by doubling, then wrap.
            std::string wrapped = "\"";
            for (char c : query) {
                if (c == '"') wrapped += '"';
                wrapped += c;
            }
            wrapped += '"';
            fts_query = std::move(wrapped);
        }

        auto stmt = db_->prepare(R"sql(
            SELECT n.key, n.value, n.tags, n.updated_at,
                   snippet(notes_fts, 1, '<<', '>>', '…', 12) AS snip,
                   bm25(notes_fts)                            AS score
            FROM notes_fts
            JOIN notes n ON n.rowid = notes_fts.rowid
            WHERE notes_fts MATCH ?1
            ORDER BY score
            LIMIT ?2
        )sql");
        stmt.bind(1, fts_query);
        stmt.bind(2, limit);

        json hits = json::array();
        while (stmt.step() == SQLITE_ROW) {
            hits.push_back({
                {"key",       stmt.text(0)},
                {"value",     stmt.text(1)},
                {"tags",      stmt.is_null(2) ? "" : stmt.text(2)},
                {"updatedAt", stmt.i64(3)},
                {"snippet",   stmt.text(4)},
            });
        }
        std::string summary = "found " + std::to_string(hits.size())
                            + " match(es) for: " + query;
        return ok_json(json{{"matches", std::move(hits)}},
                       std::move(summary));
    }

    /// `delete` is destructive enough that we route confirmation
    /// through MCP elicitation when the key looks like a glob OR
    /// the explicit `confirm` flag isn't set. Returns either the
    /// successful deletion message or a refused-by-user error.
    mcp::CallToolResult del(const json& args, mcp::Server& server) {
        if (readonly_) return err_text("notes/delete: server is read-only");
        const std::string key      = args.value("key", std::string{});
        const bool        confirm  = args.value("confirm", false);
        if (key.empty()) return err_text("notes/delete: `key` is required");

        const bool is_glob = key.find_first_of("*%") != std::string::npos;

        if (is_glob || !confirm) {
            // Ask the human via the client.
            auto fut = server.elicit(mcp::ElicitFormRequestParams{
                .message = "Delete note(s) matching key \"" + key + "\"?"
                           " This cannot be undone.",
                .requested_schema = json{
                    {"type", "object"},
                    {"properties", {
                        {"confirm", {
                            {"type", "boolean"},
                            {"description",
                             "Set true to delete; false / decline to abort."},
                        }},
                    }},
                    {"required", json::array({"confirm"})},
                },
            });
            mcp::ElicitResult er;
            try { er = fut.get(); }
            catch (const std::exception& e) {
                return err_text(std::string{"notes/delete: elicitation failed: "}
                                + e.what());
            }
            if (er.action != mcp::ElicitAction::accept ||
                !er.content.has_value() ||
                !er.content->value("confirm", false)) {
                return err_text("notes/delete: cancelled by user");
            }
        }

        // Run the delete (LIKE if it has wildcards, else equality).
        std::string sql = is_glob
            ? "DELETE FROM notes WHERE key LIKE ?1"
            : "DELETE FROM notes WHERE key = ?1";
        auto stmt = db_->prepare(sql);
        // Translate '*' → '%' for the LIKE form.
        std::string bind_key = key;
        for (char& c : bind_key) if (c == '*') c = '%';
        stmt.bind(1, bind_key);
        if (stmt.step() != SQLITE_DONE) {
            return err_text(std::string{"notes/delete: "} +
                            sqlite3_errmsg(db_->handle()));
        }
        const int n = sqlite3_changes(db_->handle());
        return ok_text(std::to_string(n) + " note(s) deleted");
    }

    mcp::CallToolResult info(const json&) {
        // count rows + size on disk + version stamp.
        std::int64_t row_count = 0;
        {
            auto stmt = db_->prepare("SELECT COUNT(*) FROM notes");
            if (stmt.step() == SQLITE_ROW) row_count = stmt.i64(0);
        }
        json structured = {
            {"path",         db_path_},
            {"readOnly",     readonly_},
            {"noteCount",    row_count},
            {"sqliteVersion", sqlite3_libversion()},
        };
        std::string summary = std::to_string(row_count)
                            + " note(s) at " + db_path_;
        return ok_json(std::move(structured), std::move(summary));
    }

    void set_db_path(std::string p) { db_path_ = std::move(p); }

    std::shared_ptr<Db> db() const { return db_; }

private:
    std::shared_ptr<Db> db_;
    bool                readonly_;
    std::string         db_path_;
};

// =====================================================================
// Resources
// =====================================================================

mcp::ReadResourceResult read_all_notes(NotesService& svc) {
    json items = json::array();
    auto stmt = svc.db()->prepare(
        "SELECT key, value, tags, updated_at FROM notes "
        "ORDER BY key ASC");
    while (stmt.step() == SQLITE_ROW) {
        items.push_back({
            {"key",       stmt.text(0)},
            {"value",     stmt.text(1)},
            {"tags",      stmt.is_null(2) ? "" : stmt.text(2)},
            {"updatedAt", stmt.i64(3)},
        });
    }
    json doc = {{"notes", std::move(items)}};
    return mcp::ReadResourceResult{
        .contents = { mcp::TextResourceContents{
            .uri       = "notes://all",
            .mime_type = "application/json",
            .text      = doc.dump(2),
        }},
    };
}

// =====================================================================
// Signal handling (mirrors the calculator example)
// =====================================================================

std::atomic<mcp::Server*> g_server{nullptr};
std::atomic_flag g_shutdown_requested = ATOMIC_FLAG_INIT;
extern "C" void on_signal(int) {
    g_shutdown_requested.test_and_set(std::memory_order_release);
}

// =====================================================================
// CLI parsing
// =====================================================================

struct Cli {
    std::string db_path;
    bool        readonly = false;
};

Cli parse_cli(int argc, char** argv) {
    Cli cli;
    cli.db_path = default_db_path();
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        if (a == "--db" && i + 1 < argc) {
            cli.db_path = argv[++i];
        } else if (a == "--readonly") {
            cli.readonly = true;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "mcp-notes-server — persistent SQLite-backed scratchpad over MCP\n\n"
                "usage: %s [--db PATH] [--readonly]\n\n"
                "  --db PATH    SQLite database (default: %s)\n"
                "  --readonly   block notes/write and notes/delete\n\n"
                "wires onto stdio. logs to stderr.\n",
                argv[0], default_db_path().c_str());
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            std::exit(2);
        }
    }
    return cli;
}

}  // namespace

int main(int argc, char** argv) {
    mcp::set_log_level(mcp::LogLevel::info);
    const Cli cli = parse_cli(argc, argv);

    std::shared_ptr<Db> db;
    try {
        db = std::make_shared<Db>(cli.db_path, cli.readonly);
        if (!cli.readonly) db->exec(kSchemaSql);
    } catch (const SqliteError& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return EXIT_FAILURE;
    }

    auto svc = std::make_shared<NotesService>(db, cli.readonly);
    svc->set_db_path(cli.db_path);

    mcp::Server server{ mcp::Implementation{
        .name        = "mcp-notes",
        .version     = "0.1.0",
        .description = "Persistent SQLite-backed scratchpad with FTS5 search.",
    }};
    server.set_instructions(
        "Persistent notes for the assistant. Use `notes/write` to save a "
        "key→value pair, `notes/read` to fetch by exact key, `notes/search` "
        "for full-text search, `notes/list` for prefix/tag filtering, and "
        "`notes/delete` to remove a note (with elicitation-confirmed glob "
        "deletes). The `notes://all` resource exposes the full set as JSON.");

    auto write_schema = json{
        {"type", "object"},
        {"properties", {
            {"key",   {{"type", "string"},
                       {"description", "stable identifier; max 256 chars"}}},
            {"value", {{"type", "string"},
                       {"description", "free-form note body"}}},
            {"tags",  {{"type", "string"},
                       {"description", "comma-separated, lowercased tags"}}},
        }},
        {"required", json::array({"key", "value"})},
    };
    server.tool("notes/write", write_schema,
        [svc](const json& args) { return svc->write(args); },
        "Write or update a note",
        "UPSERT a note under `key`. `value` is the note body; "
        "`tags` is optional comma-separated.");

    auto read_schema = json{
        {"type", "object"},
        {"properties", {{"key", {{"type", "string"}}}}},
        {"required", json::array({"key"})},
    };
    server.tool("notes/read", read_schema,
        [svc](const json& args) { return svc->read(args); },
        "Read a note by key",
        "Fetch a single note. Returns the value plus tags + timestamps.");

    auto list_schema = json{
        {"type", "object"},
        {"properties", {
            {"tag",    {{"type", "string"},
                        {"description", "filter to notes tagged this"}}},
            {"prefix", {{"type", "string"},
                        {"description", "filter to keys with this prefix"}}},
            {"limit",  {{"type", "integer"},
                        {"description", "default 50; max 1000"}}},
        }},
    };
    server.tool("notes/list", list_schema,
        [svc](const json& args) { return svc->list(args); },
        "List notes",
        "List note keys, optionally filtered by tag or key prefix. "
        "Sorted by updatedAt DESC.");

    auto search_schema = json{
        {"type", "object"},
        {"properties", {
            {"query", {{"type", "string"},
                       {"description", "FTS5 query — words match anywhere in key/value/tags"}}},
            {"limit", {{"type", "integer"},
                       {"description", "default 25; max 200"}}},
        }},
        {"required", json::array({"query"})},
    };
    server.tool("notes/search", search_schema,
        [svc](const json& args) { return svc->search(args); },
        "Full-text search",
        "BM25-ranked FTS5 search over key+value+tags. Matches are "
        "highlighted in the `snippet` field.");

    auto delete_schema = json{
        {"type", "object"},
        {"properties", {
            {"key",     {{"type", "string"},
                         {"description", "exact key, or a `*` glob"}}},
            {"confirm", {{"type", "boolean"},
                         {"description",
                          "skip the elicitation prompt for non-glob keys"}}},
        }},
        {"required", json::array({"key"})},
    };
    server.tool("notes/delete", delete_schema,
        [svc, &server](const json& args) { return svc->del(args, server); },
        "Delete a note",
        "Removes a note. Glob (e.g. `db.*`) deletes are routed through "
        "MCP elicitation for human confirmation.");

    auto info_schema = json{
        {"type", "object"},
        {"properties", json::object()},
    };
    server.tool("notes/info", info_schema,
        [svc](const json& args) { return svc->info(args); },
        "Server info",
        "Report db path, note count, and SQLite version.");

    // Resource: full notes dump as JSON.
    server.resource(mcp::Resource{
        .uri         = "notes://all",
        .name        = "all-notes",
        .title       = "All notes",
        .description = "Every note as a JSON document",
        .mime_type   = "application/json",
    },
    [svc](const std::string&) { return read_all_notes(*svc); });

    g_server.store(&server, std::memory_order_release);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    std::atomic<bool> supervisor_done{false};
    std::thread supervisor([&]() {
        while (!supervisor_done.load(std::memory_order_acquire)) {
            if (g_shutdown_requested.test(std::memory_order_acquire)) {
                if (auto* s = g_server.load(std::memory_order_acquire)) s->stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    });

    MCP_LOG_INFO("mcp-notes-server starting; db=" + cli.db_path
                 + (cli.readonly ? " (read-only)" : ""));
    server.run(std::make_unique<mcp::StdioTransport>());
    MCP_LOG_INFO("mcp-notes-server stopped");

    supervisor_done.store(true, std::memory_order_release);
    if (supervisor.joinable()) supervisor.join();
    return EXIT_SUCCESS;
}
