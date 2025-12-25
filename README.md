# desi

A **minimal, deterministic C client for LLM APIs**, built directly on top of **`jstok`**.

`desi` is designed for **systems programmers** who want full control over parsing, streaming, and protocol behavior without pulling in a JSON DOM, a scripting runtime, or a heavyweight SDK.

It also ships as an **MCP server**, exposing the same core engine to agent runtimes and external tooling.

---

## Goals

* **Zero JSON DOM**  
  Uses `jstok` strictly as a tokenizer + validator

* **Streaming-first**  
  Native SSE handling with incremental parsing

* **Tool-call correct**  
  Fully supports fragmented tool arguments in streams

* **Protocol honest**  
  Matches real OpenAI-style semantics validated by `llm_power`

* **Embeddable**  
  No global state, no background threads, no hidden allocs

* **Dual use**  
  Works as:
  * a C library
  * a standalone MCP server

---

## Non-goals

* No generic JSON object model
* No opinionated retry or policy engine
* No automatic message inference
* No transport magic beyond HTTP

---

## Architecture Overview

```

┌──────────────┐
│ Application  │
└──────┬───────┘
│
┌──────▼────────┐
│ desi public   │
│ API (llm.h)  │
└──────┬────────┘
│
├───────────────────────────┐
│ Protocol Semantics Layer  │
│                           │
│ • chat / completions      │
│ • streaming orchestration │
│ • tool loop runner        │
│ • response_format         │
└──────┬────────────────────┘
│
├───────────────────────────┐
│ JSON read/write           │
│                           │
│ • tiny JSON writer        │
│ • jstok-based extractors  │
└──────┬────────────────────┘
│
├───────────────────────────┐
│ Streaming (SSE)           │
│                           │
│ • incremental buffering   │
│ • jstok_sse_next()        │
│ • delta + tool assembly   │
└──────┬────────────────────┘
│
├───────────────────────────┐
│ Transport                 │
│                           │
│ • pluggable vtable        │
│ • libcurl backend         │
└──────┬────────────────────┘
│
┌──────▼────────┐
│ HTTP server   │
└───────────────┘

```

---

## Repository Layout

```

include/llm/
llm.h                 public API

src/
llm_transport_curl.c  HTTP backend
llm_json_write.c     minimal JSON writer
llm_json_read.c      jstok-based extractors
llm_sse.c            streaming + SSE framing
llm_chat.c
llm_completions.c
llm_tools.c
mcp_server.c         MCP server implementation

third_party/
jstok.h              vendored tokenizer

tests/
llmctl.c              mirrors llm_power expectations

````

---

## jstok Integration Contract

`jstok` is used **only** for:

* Tokenizing response JSON
* Extracting fields via paths
* Validating JSON fragments
* Scanning SSE `data:` frames

It is **not** used for:

* Request construction
* Object ownership
* Protocol logic
* Schema enforcement

This keeps parsing predictable and allocation-free.

---

## Streaming Model

* Transport delivers arbitrary byte chunks
* `llm_sse.c` maintains a rolling buffer
* `jstok_sse_next()` yields complete `data:` payloads
* Each payload is parsed independently
* Tool-call argument fragments are accumulated per call index
* `[DONE]` cleanly terminates the stream

Fragmented tool arguments are reassembled exactly as sent.

---

## Tool Call Semantics

`desi` fully implements the modern tool protocol:

* Multiple tool calls per turn
* Fragmented streaming arguments
* Stable call indexing
* Optional `id` handling
* Deterministic loop execution

Tool execution is explicit and caller-controlled.

---

## response_format Support

Supported request formats:

* `json_object`
* `json_schema`

Validation behavior:

* Returned content is re-parsed with `jstok`
* Invalid JSON is rejected immediately
* Optional shallow shape checks are possible but not required

This matches server-side guarantees while keeping the client strict.

---

## MCP Server Mode

`desi` can run as an **MCP server**, exposing:

* Chat
* Streaming chat
* Tool execution
* Structured JSON responses

The MCP server:

* Uses the same core engine
* Adds no extra parsing layers
* Emits structured JSON only
* Is suitable for agent runtimes and orchestration tools

This makes `desi` usable both as a **library** and a **process-level capability provider**.

---

## Building

```sh
meson setup build
meson compile -C build
````

---

## Testing

```sh
./build/tests/llmctl
```

All tests validate behavior by:

* Parsing responses with `jstok`
* Asserting extracted fields
* Verifying streaming and tool correctness

---

## Examples

See `examples/` for:

* Non-stream chat
* Streaming SSE
* Tool loop execution
* MCP server invocation

---

## License

MIT
