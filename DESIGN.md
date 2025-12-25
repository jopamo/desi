# DESIGN.md

This document defines the **architectural truth** of `desi`.

If the implementation diverges from this document, **the implementation is wrong**.

This file describes **data structures, ownership, and control flow**, not usage.

---

## Scope

This design covers:

* Core client data structures
* Streaming and SSE state
* Tool-call accumulation
* JSON read/write boundaries
* MCP server mapping

It intentionally does **not** describe:

* CLI UX
* Configuration files
* Build system details
* Transport internals beyond contracts

---

## High-Level Data Flow

```

HTTP bytes
↓
SSE buffer
↓
jstok_sse_next
↓
JSON tokenization
↓
Typed extraction helpers
↓
Protocol state machine
↓
User callbacks / MCP responses

````

No stage mutates data owned by another stage.

---

## Core Types

### llm_client_t

The client is a **pure configuration + backend handle**.

```c
typedef struct {
    const char* base_url;
    const char* model;

    llm_http_backend_t* http;
    void* http_user;

    uint32_t timeout_ms;
    uint32_t max_retries;

    llm_header_t* headers;
    size_t header_count;
} llm_client_t;
````

Rules:

* owns nothing except header array if provided
* immutable after initialization
* safe for reuse across requests
* thread safety is caller-managed

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
* body buffer owned by caller or JSON writer
* transport may not mutate

---

### llm_response_t

Represents a **single HTTP response**, streamed or not.

```c
typedef struct {
    int status;
    const char* body;
    size_t body_len;
} llm_response_t;
```

Rules:

* body lifetime defined by transport
* spans extracted from this buffer only
* never cached beyond call scope

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

* never reallocates unless explicitly configured
* overflow is sticky
* caller checks success after write sequence
* writer never validates output

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

* tokens reference `json` buffer
* view owns tokens only
* tokens are invalid once buffer is freed
* no mutation allowed

---

### Extraction Result Types

```c
typedef struct {
    const char* ptr;
    size_t len;
} llm_span_t;
```

Rules:

* spans never own memory
* empty span is `{NULL, 0}`
* validity depends on backing buffer

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

* buffer must remain stable between calls
* `pos` is the read cursor for `jstok_sse_next`
* compaction preserves unread bytes
* buffer growth is bounded

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

---

## Tool Accumulation

### llm_tool_accumulator_t

Accumulates one tool call across chunks.

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

* `args_buf` is append-only
* argument fragments are concatenated verbatim
* JSON is validated only after completion
* order is defined by stream index

---

## Protocol-Level Structures

### llm_message_t

Represents a chat message provided by the caller.

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
* tool results appended explicitly

---

## MCP Server Mapping

The MCP server is a **thin adapter**.

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

---

## Ownership Summary

| Resource       | Owner        |
| -------------- | ------------ |
| HTTP buffers   | transport    |
| JSON tokens    | json view    |
| spans          | non-owning   |
| stream buffers | stream state |
| tool args      | accumulator  |
| messages       | caller       |

Violating ownership rules is a correctness bug.

---

## Invariants

These must always hold:

* no hidden allocation
* no token survives its buffer
* no tool executes implicitly
* no stream parses incomplete JSON
* no protocol logic in transport

---

## Final Statement

`desi` is intentionally strict.

It prefers:

* explicit state
* visible ownership
* boring control flow

Any design change that weakens these properties is unacceptable.
