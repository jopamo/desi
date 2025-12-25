<div style="background-color:#1e1e1e; padding:1em; display:inline-block; border-radius:8px; text-align:left;">
  <img src=".github/desi.png" alt="desi logo" width="300" style="display:block; margin:0;">
</div>

A **minimal, deterministic C client for LLM APIs**, built directly on top of **`jstok`**.

`desi` is designed for **systems programmers** who want full control over parsing, streaming, and protocol behavior without pulling in a JSON DOM, a scripting runtime, or a heavyweight SDK.

It also ships as an **MCP server**, exposing the same core engine to agent runtimes and external tooling.

---

## Where `desi` Fits

`desi` is the **boring, correct transport + protocol binding** layer in a larger stack.

```

ET (identity, authorization, audit, policy)
└── uses desi for outbound HTTP(S) calls
├── llama-server (stateless inference appliance)
├── registries
├── internal APIs
└── MCP servers

desi = HTTP(S) client + SSE framing + zero-DOM JSON + protocol semantics

```

`desi` participates in **authentication** (TLS verification, optional mTLS via explicit configuration) and never **authorization**.

* ET decides who is allowed and what is permitted
* desi ensures bytes, parsing, and protocol semantics are correct and explicit
* llama-server generates tokens and nothing else

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

* **Mechanically bounded**  
  Explicit limits for request/response/stream buffers and tool args

---

## Non-goals

* No generic JSON object model
* No opinionated retry or policy engine
* No automatic message inference
* No orchestration, scheduling, or caching
* No transport magic beyond HTTP contracts
* No authorization decisions

---

## Architecture Overview

```

┌──────────────┐
│ Application  │
└──────┬───────┘
│
┌──────▼────────┐
│ desi public   │
│ API (llm.h)   │
└──────┬────────┘
│
├───────────────────────────┐
│ Protocol Semantics Layer  │
│                           │
│ • chat / completions      │
│ • streaming orchestration │
│ • tool loop runner        │
│ • response_format         │
│ • finish_reason rules     │
└──────┬────────────────────┘
│
├───────────────────────────┐
│ JSON read/write           │
│                           │
│ • tiny JSON writer        │
│ • jstok-based extractors  │
│ • span-based results      │
└──────┬────────────────────┘
│
├───────────────────────────┐
│ Streaming (SSE)           │
│                           │
│ • incremental buffering   │
│ • jstok_sse_next()        │
│ • delta + tool assembly   │
│ • [DONE] termination      │
└──────┬────────────────────┘
│
├───────────────────────────┐
│ Transport                 │
│                           │
│ • pluggable vtable        │
│ • libcurl backend         │
│ • TLS verify + mTLS       │
└──────┬────────────────────┘
│
┌──────▼────────┐
│ Remote server │
└───────────────┘

```

Layer rules:

* transport is a byte pump
* SSE does framing only
* JSON does tokenization and extraction only
* protocol owns semantics and validation

No layer mutates data owned by another layer.

---

## Repository Layout

```

include/llm/
llm.h                 public API

src/
llm_transport_curl.c  HTTP backend
llm_json_write.c      minimal JSON writer
llm_json_read.c       jstok-based extractors
llm_sse.c             streaming + SSE framing
llm_chat.c
llm_completions.c
llm_tools.c
mcp_server.c          MCP server implementation

third_party/
jstok.h               vendored tokenizer

tests/
llmctl.c              mirrors llm_power expectations

````

---

## jstok Integration Contract

`jstok` is used only for:

* Tokenizing response JSON
* Extracting fields via paths
* Validating JSON fragments
* Scanning SSE `data:` frames

It is not used for:

* Request construction
* Object ownership
* Protocol logic
* Schema enforcement beyond local validation
* Caching or memoization

This keeps parsing predictable and allocation-free.

---

## Memory and Ownership Model

`desi` is span-first.

* response JSON buffers are owned by the transport
* all extracted values are spans into those buffers
* copying is explicit and opt-in
* no hidden allocation in extraction helpers

Streaming buffers:

* are owned by per-request stream state
* are bounded and compacted
* must remain stable across partial reads

If a function allocates, it must be obvious from the API.

---

## Error Model

Errors are values, not logs.

* public APIs return error codes
* errors identify the failing stage (transport, SSE framing, parse, protocol)
* partial success is not allowed unless explicitly designed
* server incorrectness is treated as a normal error path

`desi` does not hide failures.

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

`desi` implements the modern tool protocol:

* Multiple tool calls per turn
* Fragmented streaming arguments
* Stable call indexing
* Optional `id` handling
* Deterministic loop execution
* Explicit caller-controlled tool execution

Tool execution is never implicit.

---

## response_format Support

Supported request formats:

* `json_object`
* `json_schema`

Validation behavior:

* returned content is re-parsed with `jstok`
* invalid JSON is rejected immediately
* any additional shape checks must be explicit and bounded

`desi` treats response_format promises as untrusted until validated.

---

## TLS and mTLS

Defaults:

* TLS verification on
* system trust store by default
* hostname verification on

Optional:

* mTLS via explicit client cert + key configuration

`desi` does not decide who is authorized.

It only ensures the cryptographic session is correct per configuration.

---

## MCP Server Mode

`desi` can run as an MCP server, exposing:

* chat
* streaming chat
* tool loop coordination
* structured JSON responses

The MCP server:

* uses the same core engine
* adds no extra parsing layers
* maps errors directly
* preserves streaming semantics

If MCP behavior differs from library behavior, that is a bug.

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

* parsing responses with `jstok`
* asserting extracted fields and spans
* verifying streaming and tool correctness
* exercising failure paths and limits

---

## Examples

See `examples/` for:

* non-stream chat
* streaming SSE
* tool loop execution
* MCP server invocation

---

## License

MIT

