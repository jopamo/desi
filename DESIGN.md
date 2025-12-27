# DESIGN.md

This document defines the **architectural truth** of `desi`.

If the implementation diverges from this document, **the implementation is wrong**.

This file describes **data structures, ownership, and control flow**, not usage.

---

## Where `desi` Fits

`desi` is a **strict C foundation for LAN agent systems** made of:

* **`libdesi`**: HTTP(S) client + TLS/mTLS + SSE framing + zero-DOM JSON + explicit protocol bindings
* **`desid`**: an HTTP gateway daemon that talks to LLM backends using `libdesi`
* **`agentd`**: an orchestrator daemon that runs bounded agent state machines and streams run events over SSE
* **`mcpd`**: tool server daemons that expose explicit capabilities over HTTP

In the larger system, ET can later front these components, but ET is not required for correctness inside this repo

```

ET (identity, authorization, audit, policy)
└── fronts agentd/desid
├── agentd (runs + tool coordination)
├── desid  (LLM gateway)
└── mcpd-* (tool servers)
|
└── libdesi (transport + SSE + jstok parsing)

```

**Hard invariant:** only `desid` is permitted to connect to LLM backends

`desi` participates in **authentication** (TLS verification, optional mTLS by explicit config) but never **authorization**

`desi` must remain:

* small
* deterministic
* allocation-explicit
* zero-DOM
* layered

---

## Scope

This design covers:

* Core library contracts and data structures (`libdesi`)
* SSE parsing and emission rules
* JSON read/write boundaries (zero DOM)
* LLM protocol bindings as mechanical state machines
* `desid` gateway responsibilities and control flow
* `agentd` run/step model and SSE event stream
* `mcpd` tool server mapping and boundaries

It intentionally does not describe:

* CLI UX
* configuration formats
* build system details
* transport internals beyond contracts
* authorization, rate limiting, or fleet policy (ET territory)

---

## High-Level Data Flows

### LLM call path

```

HTTP request to desid
↓
protocol binding (request validation + construction)
↓
transport (HTTP/TLS byte pump)
↓
SSE framing (if streaming)
↓
JSON tokenization (jstok)
↓
typed extraction helpers (spans, no allocation)
↓
protocol state machine (explicit)
↓
HTTP response from desid (JSON or SSE)

```

### Orchestration path

```

HTTP request to agentd
↓
run state machine (bounded steps)
↓
step executes:

* LLM step: agentd → desid
* tool step: agentd → mcpd-*
  ↓
  agentd emits run events as SSE

````

No stage mutates data owned by another stage

---

## Core Contracts

### Transport Contract (libdesi)

Transport provides:

* request execution
* status code and headers
* streaming bytes in order
* response body lifetime rules
* TLS verification and client cert plumbing as configured by caller

Transport must not:

* parse JSON
* interpret SSE semantics
* retry implicitly
* mutate request bodies
* assume server correctness

Transport is a byte pump with metadata

---

### SSE Contract (libdesi)

SSE provides:

* UTF-8 decode and one leading BOM strip
* line splitting using CRLF, LF, or CR
* event framing into:
  - data buffer
  - event type buffer
  - last event id buffer
  - reconnection time (retry)
* dispatch on blank lines only

SSE must not:

* parse JSON semantics
* interpret protocol markers like `[DONE]`
* allocate implicitly
* lose unread bytes during compaction

SSE is framing only

---

### Parsing Contract (libdesi)

Parsing provides:

* JSON tokenization
* span-based extraction helpers
* fragment validation (tool args and similar payloads)

Parsing must not:

* allocate implicitly
* enforce protocol decisions
* cache cross-request state
* infer schemas
* hide errors

---

### Protocol Contract (libdesi)

Protocol provides:

* request construction
* response interpretation
* explicit validation of what matters
* streaming state machines

Protocol must not:

* rely on transport quirks
* assume server correctness
* grow hidden background state
* become a policy engine
* execute tools

---

### Daemon Contracts

#### desid contract

desid provides:

* a stable HTTP API for LLM calls
* optional SSE streaming responses
* mechanical limits applied consistently
* error surfaces that preserve stage

desid must not:

* execute tool loops
* decide tool policy
* interpret tool output beyond protocol requirements
* call other services behind callers’ backs

#### agentd contract

agentd provides:

* run lifecycle management
* explicit bounded step machine
* tool coordination and loop limits
* run event stream as SSE

agentd must not:

* talk to LLM backends directly
* embed LLM credentials or backend addresses
* hide failure via implicit retries

#### mcpd contract

mcpd provides:

* explicit tools exposed over HTTP
* deterministic bounded outputs

mcpd must not:

* talk to LLM backends
* contain orchestration logic

---

## Core Types (libdesi)

### llm_client_t

Configuration + backend handle

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

libdesi must not store per-request state inside the client

---

### llm_request_t

Internal, stack-constructed request description

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

Represents a single HTTP response, streamed or not

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

Append-only JSON builder

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

Writer APIs must make allocation behavior obvious

---

## JSON Reader Structures

### llm_json_view_t

A parsed JSON view over an immutable buffer

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

## Streaming Structures (libdesi)

### llm_sse_buffer_t

Stateful accumulator for streaming responses

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
* `pos` is the read cursor for SSE parsing
* compaction preserves unread bytes
* growth is bounded by `client->max_sse_buffer_bytes`
* the SSE layer does framing only

The SSE layer must never parse JSON semantics

---

### llm_sse_event_t

A framed SSE event ready for higher-layer interpretation

```c
typedef struct {
    llm_span_t event_type;   /* empty means "message" */
    llm_span_t data;         /* data buffer after trailing LF removal */
    llm_span_t last_event_id;/* last id seen, may be empty */
} llm_sse_event_t;
```

Rules:

* spans are views into the SSE parser’s stable buffers for the lifetime of the callback
* data must be produced by the SSE rules:

  * `data:` lines append value + LF
  * trailing LF removed before dispatch
  * dispatch on blank line only
* if data is empty, dispatch is suppressed and buffers reset

---

### llm_stream_state_t

Per-request streaming state for LLM responses

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

If tool accumulation is disabled, `tools` is NULL and `tool_count` is 0

---

## Tool Accumulation (libdesi)

### llm_tool_accumulator_t

Accumulates one tool call across streamed chunks

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

Tool args are opaque to libdesi beyond JSON validity

---

## Protocol-Level Structures (libdesi)

### llm_message_t

Chat message provided by the caller

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

Tool definition passed to the protocol layer

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
* libdesi never executes tools

---

## Orchestration Structures (agentd)

agentd owns orchestration state and does not belong in libdesi

### agent_run_t

A run is a bounded state machine with an append-only event log

```c
typedef struct {
    uint64_t run_id;

    uint32_t max_steps;
    uint32_t max_tool_calls;
    uint32_t timeout_ms;

    uint32_t step_index;
    uint32_t tool_calls_used;

    bool cancelled;
    bool done;
    int  last_error;

    /* opaque caller payloads, stored explicitly if enabled */
    void* user;
} agent_run_t;
```

Rules:

* all counters are bounded and checked before every step transition
* no implicit retries
* determinism is defined by:

  * same inputs
  * same tool outputs
  * same model outputs
    producing the same state transitions

### agent_event_t

Events are what SSE transports

```c
typedef struct {
    const char* type;     /* "run.started", "step.started", "tool.completed", ... */
    const char* json;     /* serialized JSON payload */
    size_t json_len;
} agent_event_t;
```

Rules:

* SSE emits events with `event:` set to `type` and `data:` carrying `json`
* event emission is ordered and append-only
* clients must be able to reconstruct progress from the stream alone

agentd uses SSE framing rules identical to libdesi SSE writer rules

---

## Tool Server Structures (mcpd)

mcpd is a tool capability server and does not include orchestration

### mcp_tool_t

```c
typedef struct {
    const char* name;
    const char* description;
    const char* input_schema_json;
} mcp_tool_t;
```

Rules:

* input schema JSON must be valid JSON
* tool outputs are bounded and deterministic
* mcpd must never call LLMs

---

## HTTP Surface Summary

### desid

* accepts LLM calls over HTTP
* streams responses using SSE for streaming calls
* is the only component that talks to LLM backends

### agentd

* accepts run creation and control
* emits run events as SSE
* calls desid and mcpd as needed

### mcpd

* exposes tools over HTTP
* returns bounded results

---

## Ownership Summary

| Resource         | Owner                        |
| ---------------- | ---------------------------- |
| HTTP buffers     | transport or request context |
| response headers | transport                    |
| JSON tokens      | json view                    |
| spans            | non-owning                   |
| stream buffers   | stream state                 |
| tool args buf    | accumulator                  |
| messages         | caller                       |
| client config    | caller                       |
| run state        | agentd                       |
| tool state       | mcpd                         |

Violating ownership rules is a correctness bug

---

## Invariants

These must always hold:

* only desid reaches LLM backends
* no hidden allocation
* no token survives its buffer
* no tool executes implicitly
* no stream dispatches incomplete events
* no protocol logic in transport
* no SSE semantics beyond framing
* all limits are enforced mechanically and consistently
* failures are explicit and returned as error codes

---

## Final Statement

`desi` is intentionally strict

It prefers:

* explicit state
* visible ownership
* boring control flow
* mechanical limits over “smart” behavior

Any design change that weakens these properties is unacceptable
