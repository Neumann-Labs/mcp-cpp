# mcp-cpp benchmarks

Microbenchmarks isolating the SDK's *own* per-call cost from network/OS noise,
plus footprint/startup measurements that motivate a native MCP implementation.

## Why these numbers matter

MCP's primary transport [launches the server "as a subprocess"](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
on the user's machine, so the server's runtime weight is paid *per process,
locally*. Python and Node MCP servers therefore drag a full language runtime
(tens of MB RSS, tens-to-hundreds of ms cold start, GIL/single-thread
concurrency ceilings, GC jitter) into the host's process tree. A C++ SDK can
compile the client and server **straight into the host binary** — in-process,
no interpreter, no IPC marshalling, one static artifact — which is the only
viable shape for embedding LLM tool-calling inside footprint- and
latency-constrained native software (game engines, IDEs, trading systems,
robotics, DB engines, audio/DSP).

## What's measured

`mcp_bench.cpp` wires a `Server` and `Client` together over the in-process
paired transport from the test suite (`mcp::test::make_in_memory_pair()`), so
the only thing on the clock is the SDK: serialize → in-proc queue → server
dispatch thread → parse → session worker → handler → serialize → queue →
client dispatch thread → parse → resolve future. No sockets, no pipes, no
subprocess.

1. **`tools/call` round-trip** — full request/response through the stack.
2. **`ping` round-trip** — the protocol floor (empty payload).
3. **JSON-RPC codec** — `serialize_message().dump()` and
   `json::parse() → parse_message()` in isolation (transport-independent).

## Build & run (standalone, against the prebuilt static lib)

```sh
c++ -std=c++20 -O2 -DNDEBUG \
  -I include -I tests -I build-rel/_deps/nlohmann_json-src/include \
  bench/mcp_bench.cpp tests/in_memory_transport.cpp \
  build-rel/libmcp.a -o /tmp/mcp_bench
/tmp/mcp_bench 100000      # iterations (default 100000)
```

## Results

Measured on Apple Silicon (arm64, 8 hardware threads), Apple clang 15, `-O2
-DNDEBUG`, single client thread. Latency is wall-clock per `call().get()`.

### In-process SDK overhead

| Benchmark            | Throughput        | p50    | mean   | p99    | p99.9  |
| -------------------- | ----------------- | ------ | ------ | ------ | ------ |
| `tools/call` (add)   | ~30,000 calls/sec | 31 µs  | 36 µs  | 108 µs | 221 µs |
| `ping` (empty)       | ~34,000 calls/sec | 26 µs  | 29 µs  | 72 µs  | 160 µs |

| Codec (101-byte `tools/call` frame) | per op  | rate          |
| ----------------------------------- | ------- | ------------- |
| serialize (typed → json → string)   | 1.32 µs | ~760k ops/sec |
| parse (string → json → typed)       | 2.03 µs | ~492k ops/sec |

The round-trip latency (~31 µs) is dominated by OS thread-scheduling: each call
crosses condition-variable handoffs between the client dispatch thread, the
session read thread, and the server worker. The SDK's pure CPU cost per call is
the codec (~3.3 µs combined).

> **Optimization applied.** `Client::call_tool` (and the other request wrappers)
> used to spawn one `std::thread` per call via `std::async(std::launch::async)`
> just to convert the result JSON to a typed value. That work now runs as a
> typed continuation on the session read thread (`Session::send_request_for<T>`),
> with no per-call thread. Measured effect on this machine: `tools/call`
> **~24,000 → ~30,000 calls/sec (+~25%)** and **p50 ~39 µs → ~31 µs (−~21%)**;
> `.wait_for()` semantics are preserved.

**Context:** an LLM tool call is gated by model latency measured in *seconds*.
At ~31 µs of SDK overhead per call, the library adds < 0.01% to a round-trip —
it is never the bottleneck.

### Footprint & startup (native vs interpreted, same machine)

| Metric           | C++ stdio server | Python 3.14         | Node 26  |
| ---------------- | ---------------- | ------------------- | -------- |
| Idle RSS         | **1.5 MB**       | 14.4 MB (bare)      | 47 MB    |
| Cold start       | **3.1 ms**       | 21 ms (`-c pass`)   | 57 ms    |
| Cold start (+stdlib) | —            | 56 ms (asyncio+json)| —        |
| Deploy artifact  | one 644 KB binary| interpreter + venv  | node + node_modules |

The C++ stdio server (a minimal `server.run(StdioTransport)`, statically linked
against `libmcp.a`, only `libc++` dynamic) is **~10× smaller RSS than bare
CPython, ~30× smaller than Node, and starts ~7× faster** than even a bare
interpreter. A real Python MCP server importing `mcp`/`anyio`/`pydantic` lands
materially higher on both axes.

> **Dependency hygiene (fixed): the HTTP transport is its own target.** The core
> `mcp` library now depends only on `nlohmann_json`; the Streamable-HTTP
> transport lives in a separate `mcp::http` target that carries the
> `httplib → OpenSSL/brotli` deps. A stdio-only server linked against `mcp::mcp`
> therefore loads **no** OpenSSL/brotli/Security at all (verify with `otool -L`).
>
> Honest accounting of the startup win: in isolation, force-linking
> OpenSSL+brotli+Security adds ~3 ms to a minimal server's load (3.0 ms → 6.1 ms
> on this machine), so dropping them roughly halves cold start. (An earlier
> "~116 ms" figure was a *measurement artifact* — the `calculator_server`
> example's signal-handling supervisor `sleep_for(100 ms)`s, so an EOF-triggered
> exit waits on that poll; it is shutdown latency, not startup.) The larger,
> durable win is the deployment story: stdio servers need no OpenSSL on the
> target and carry none of its CVE surface, and the build no longer depends on
> system brotli headers being present.

## Sources for the comparison narrative

- MCP transports (subprocess model): https://modelcontextprotocol.io/specification/2025-11-25/basic/transports
- Python GIL concurrency ceiling: https://peps.python.org/pep-0703/
- Node event-loop blocking on sync work: https://nodejs.org/learn/asynchronous-work/dont-block-the-event-loop
- Native JSON parse throughput (simdjson, GB/s): https://github.com/simdjson/simdjson
- Python startup time: https://pythondev.readthedocs.io/startup_time.html
- GC/jitter disqualifying in HFT: https://arxiv.org/pdf/2309.04259
- Unreal Python is editor-only (no shipped-build interpreter): https://dev.epicgames.com/documentation/unreal-engine/scripting-the-unreal-editor-using-python
