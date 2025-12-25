# DESIGN.md

This document defines the **architectural truth** of `desi`.

If the implementation diverges from this document, **the implementation is wrong**.

This file describes **data structures, ownership, and control flow**, not usage.

---

## Where `desi` Fits

`desi` is a **strict HTTP + TLS client and protocol binding layer**.

In the larger system:

```

ET (identity, authorization, audit, policy)
└── uses desi for outbound HTTP(S)
├── llama-server (stateless inference appliance)
├── registries
├── internal APIs
└── MCP servers

desi = transport contracts + bounded parsing + explicit protocol semantics

```

`desi` participates in **authentication** (TLS verification, optional mTLS by explicit config) but never **authorization**.

`desi` must remain:

* small
* deterministic
* allocation-explicit
* zero-DOM
* layered

---

## Scope

This design covers:

* Core client data structures
* Streaming and SSE state
* Tool-call accumulation
* JSON read/write boundaries
* Protocol state machines
* MCP server mapping

It intentionally does not describe:

* CLI UX
* Configuration formats
* Build system details
* Transport internals beyond contracts
* Authorization, rate limiting, or orchestration

---

## High-Level Data Flow

```

HTTP bytes
↓
SSE buffer (framing only)
↓
jstok_sse_next (frame boundaries)
↓
JSON tokenization (jstok)
↓
Typed extraction helpers (spans, no allocation)
↓
Protocol state machine (explicit)
↓
User callbacks / MCP responses

````

No stage mutates data owned by another stage.

All non-trivial behavior lives in the protocol layer, not transport or parsing.

---

## Core Contracts

### Transport Contract

Transport provides:

* request execution
* status code and headers
* streaming bytes in order
* response body lifetime rules
* TLS verification and client cert plumbing as configured by caller

Transport must not:

* parse JSON
* interpret SSE
* retry implicitly
* mutate request bodies
* assume server correctness

Transport is a byte pump with metadata.

---

### Parsing Contract

Parsing provides:

* SSE frame boundaries
* JSON tokenization
* span-based extraction helpers

Parsing must not:

* allocate implicitly
* enforce protocol decisions
* cache
* infer schemas
* hide errors

---

### Protocol Contract

Protocol provides:

* request construction
* response interpretation
* tool loop coordination
* explicit validation of “what matters”

Protocol must not:

* rely on transport quirks
* assume server correctness
* grow hidden background state
* become a policy engine

---

## Core Types

### llm_client_t

The client is **configuration + backend handle**.

```c
typedef struct {
    const char* base_url;
    const char* model;

    llm_http_backend_t* http;
    void* http_user;

    uint32_t timeout_ms;

    llm_header_t* headers;
    size_t header_count;

    /* Hard limits enforced mechanically, not semantically */
    size_t max_request_bytes;
    size_t max_response_bytes;
    size_t max_sse_buffer_bytes;
} llm_client_t;
````

Rules:

* owns nothing except header array if provided by caller as owned storage
* immutable after initialization
* safe for reuse across requests
* thread safety is caller-managed
* all limits are explicit and enforced consistently

`desi` must not store per-request state inside the client.

---

### llm_request_t

Internal, stack-constructed description of a request.

```c
typedef struct {
    const char* endpoint;

    const char* body;
    size_t body_len;

    bool stream;
} llm_request_t;
```

Rules:

* never heap-allocated
* body buffer is owned by caller or JSON writer buffer
* transport may not mutate request buffers
* endpoint is a static string or caller-owned

---

### llm_response_t

Represents a single HTTP response, streamed or not.

```c
typedef struct {
    int status;

    const char* body;
    size_t body_len;

    llm_header_t* headers;
    size_t header_count;
} llm_response_t;
```

Rules:

* response buffer lifetime is defined by transport
* spans extracted from this buffer only
* never cached beyond call scope
* headers are transport-owned unless explicitly copied by caller

---

## JSON Writer Structures

### llm_json_writer_t

Append-only JSON builder.

```c
typedef struct {
    char* buf;
    size_t len;
    size_t cap;

    bool overflow;
} llm_json_writer_t;
```

Rules:

* growth policy is explicit
* overflow is sticky
* caller checks success after write sequence
* writer never validates output beyond mechanical escaping
* writer output must be deterministic for equivalent inputs

Writer API must make allocation behavior obvious.

---

## JSON Reader Structures

### llm_json_view_t

A parsed JSON view over an immutable buffer.

```c
typedef struct {
    const char* json;
    size_t len;

    jstok_token_t* toks;
    size_t tok_count;
} llm_json_view_t;
```

Rules:

* tokens reference the `json` buffer
* view owns tokens only
* tokens are invalid once buffer is freed
* no mutation allowed

Token allocation strategy is explicit:

* either caller-provided token buffer
* or view-managed token buffer with explicit init/fini

---

### Spans and Typed Results

```c
typedef struct {
    const char* ptr;
    size_t len;
} llm_span_t;
```

Rules:

* spans never own memory
* empty span is `{NULL, 0}`
* validity depends on backing buffer lifetime
* spans are not NUL-terminated
* conversions to owned strings are explicit and opt-in

---

## Streaming Structures

### llm_sse_buffer_t

Stateful accumulator for streaming responses.

```c
typedef struct {
    char* buf;
    size_t len;
    size_t cap;

    size_t pos;
} llm_sse_buffer_t;
```

Rules:

* buffer remains stable between calls
* `pos` is the read cursor for `jstok_sse_next`
* compaction preserves unread bytes
* growth is bounded by `client->max_sse_buffer_bytes`
* the SSE layer does framing only

The SSE layer must never parse JSON semantics.

---

### llm_stream_state_t

Per-request streaming state.

```c
typedef struct {
    llm_sse_buffer_t sse;

    bool done;
    bool error;

    llm_tool_accumulator_t* tools;
    size_t tool_count;
} llm_stream_state_t;
```

Rules:

* allocated per stream request
* destroyed immediately on completion
* no global reuse
* no hidden global buffers

If tool accumulation is disabled, `tools` is NULL and `tool_count` is 0.

---

## Tool Accumulation

### llm_tool_accumulator_t

Accumulates one tool call across streamed chunks.

```c
typedef struct {
    llm_span_t id;
    llm_span_t name;

    char* args_buf;
    size_t args_len;
    size_t args_cap;

    bool complete;
} llm_tool_accumulator_t;
```

Rules:

* `id` and `name` are spans into the current JSON frame buffer
* `args_buf` is append-only owned storage
* argument fragments are concatenated verbatim in receive order
* JSON validation happens only after completion
* accumulation is bounded by an explicit max args size
* ordering is defined by stream index, not arrival timing

Tool args are opaque to `desi` beyond JSON validity.

---

## Protocol-Level Structures

### llm_message_t

Chat message provided by the caller.

```c
typedef struct {
    const char* role;
    const char* content;

    const char* tool_call_id;
} llm_message_t;
```

Rules:

* strings are caller-owned
* client never mutates messages
* serialization is deterministic
* tool_call_id is optional and only meaningful for tool-result messages

---

### llm_tool_t

Tool definition passed to the protocol layer.

```c
typedef struct {
    const char* name;
    const char* description;
    const char* parameters_json;
} llm_tool_t;
```

Rules:

* description is mandatory
* parameters must be valid JSON
* no schema inference performed
* `desi` never executes tools, it only coordinates the loop

---

## Tool Loop State

```c
typedef struct {
    llm_message_t* messages;
    size_t message_count;

    size_t turns;
    uint64_t recent_hashes[8];
} llm_tool_loop_t;
```

Rules:

* bounded number of turns
* loop detection via rolling hash
* messages grow monotonically
* tool results appended explicitly by the caller
* tool loop is never implicit

`desi` may provide helpers for loop coordination but never hides execution.

---

## MCP Server Mapping

The MCP server is a thin adapter.

Mapping rules:

| MCP concept | desi equivalent |
| ----------- | --------------- |
| tool call   | llm_tool_t      |
| message     | llm_message_t   |
| stream      | llm_stream_*    |

Rules:

* no duplicate parsing
* no parallel protocol logic
* MCP errors map directly to llm errors
* streaming remains streaming
* MCP must not add convenience behaviors that change semantics

If MCP behavior differs from library behavior, that is a bug.

---

## Ownership Summary

| Resource         | Owner        |
| ---------------- | ------------ |
| HTTP buffers     | transport    |
| response headers | transport    |
| JSON tokens      | json view    |
| spans            | non-owning   |
| stream buffers   | stream state |
| tool args buf    | accumulator  |
| messages         | caller       |
| client config    | caller       |

Violating ownership rules is a correctness bug.

---

## Invariants

These must always hold:

* no hidden allocation
* no token survives its buffer
* no tool executes implicitly
* no stream parses incomplete JSON
* no protocol logic in transport
* no SSE semantics beyond framing
* all limits are enforced mechanically and consistently
* failures are explicit and returned as error codes

---

## Final Statement

`desi` is intentionally strict.

It prefers:

* explicit state
* visible ownership
* boring control flow
* mechanical limits over “smart” behavior

Any design change that weakens these properties is unacceptable.
