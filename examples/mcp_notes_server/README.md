# mcp-notes-server

A SQLite + FTS5 scratchpad for Claude (or any MCP client). Plug it
into your client of choice and the LLM gains persistent,
full-text-searchable memory across sessions:

- "Remember my Postgres prod connection string under `db.prod`."
- "What's my preferred indent style?"
- "Search my notes for postgres."
- "Forget everything tagged `experiment-3`."

Storage is one SQLite database file (default
`~/.local/state/mcp-notes/notes.db`, override with `--db`). Search
runs on an FTS5 virtual table with porter stemming + unicode61
tokenisation, BM25-ranked. Destructive operations on key globs are
routed through the spec's elicitation primitive so the human gets a
confirmation prompt before anything is wiped.

## Build

The example builds against system SQLite. On macOS, that's already
available via the Xcode CLT; on Debian/Ubuntu, install the dev
package:

```sh
sudo apt install libsqlite3-dev
```

Then, from the repo root:

```sh
cmake -S . -B build -DMCP_BUILD_EXAMPLES=ON
cmake --build build --target mcp_notes_server
```

If SQLite isn't found at configure time the example silently skips
its build — the rest of the SDK is unaffected.

## Wire it into Claude Desktop

Edit `~/Library/Application Support/Claude/claude_desktop_config.json`
(macOS) or your platform's equivalent:

```json
{
  "mcpServers": {
    "notes": {
      "command": "/absolute/path/to/build/examples/mcp_notes_server/mcp_notes_server",
      "args": ["--db", "/Users/you/.notes.db"]
    }
  }
}
```

Restart Claude Desktop. The next conversation has six new tools
(`notes/write`, `notes/read`, `notes/search`, `notes/list`,
`notes/delete`, `notes/info`) and one resource (`notes://all`).

For Claude Code: `claude mcp add notes /path/to/mcp_notes_server`.

## Tools

| Tool             | Purpose |
|------------------|---------|
| `notes/write`    | UPSERT a note: `{ key, value, tags? }`. |
| `notes/read`     | Fetch one by key. |
| `notes/list`     | List notes, optionally filtered by `tag` and/or `prefix`. |
| `notes/search`   | BM25 full-text over key + value + tags. Returns highlighted snippets. |
| `notes/delete`   | Remove a note. Globs (`db.*`) and unconfirmed deletes route through elicitation. |
| `notes/info`     | Path, count, SQLite version. |

## Resources

- `notes://all` — every note as a single JSON document
  (`application/json`).

## CLI

```
mcp-notes-server [--db PATH] [--readonly]

  --db PATH    SQLite database to use (default ~/.local/state/mcp-notes/notes.db)
  --readonly   Block notes/write and notes/delete (good for sharing a curated db).
```

The server speaks MCP over stdio; logs go to stderr.

## What's interesting about it as an SDK demo

1. **Real persistence.** This isn't a toy — the same SQLite file
   stays across restarts, and the schema is robust enough to grow
   into a real personal knowledge store.

2. **Full-text search via FTS5.** A single virtual table + three
   triggers gives Claude BM25-ranked search with snippet
   highlighting. The handler quotes user queries defensively so
   typical LLM-generated patterns (`key:value`-style probes) parse
   cleanly.

3. **Elicitation for destructive ops.** `notes/delete` on a glob
   (or any delete that omits the explicit `confirm` flag) issues
   `elicitation/create` to the client, asking the human for a
   yes/no confirmation. If the human declines, the delete aborts —
   demonstrating the bidirectional MCP surface in a way that's
   actually useful, not contrived.

4. **Structured tool output.** Every read / list / search returns
   both a human summary (TextContent) and a machine-readable
   `structured_content` payload, so well-behaved clients can render
   richly without re-parsing the text.

5. **Resource integration.** `notes://all` exposes the full notes
   set as a static-feeling JSON resource — useful for clients that
   want to give the LLM read-only context dumps without burning a
   tool call.

## Try it with raw stdio

```sh
$ rm -f /tmp/demo.db
$ ( cat <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cli","version":"0"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"notes/write","arguments":{"key":"db.prod","value":"postgres://prod.example/main","tags":"db"}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"notes/search","arguments":{"query":"postgres"}}}
EOF
sleep 0.2
) | ./mcp_notes_server --db /tmp/demo.db
```

The `notes/search` response will include a `<<postgres>>://prod.example/main`
snippet in `structuredContent.matches[0].snippet`.
