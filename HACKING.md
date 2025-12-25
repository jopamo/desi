# HACKING.md

This document describes **how to work on `desi` correctly**.

If behavior in code diverges from what is described here, **the code is wrong**.

---

## Core Philosophy

`desi` is intentionally small, explicit, and hostile to abstraction creep.

Design principles:

* **No JSON DOM**
* **No implicit allocation**
* **No hidden protocol behavior**
* **No background threads**
* **No magic retries**
* **No guessing**

Every byte, token, and buffer must be explainable.

---

## Architectural Invariants

These rules are non-negotiable.

### jstok usage rules

`jstok` is used **only** for:

* tokenization
* validation
* path-based extraction
* SSE `data:` frame scanning

`jstok` must **never** be used for:

* request construction
* object ownership
* protocol decisions
* schema enforcement
* caching or memoization

If you find yourself wanting a DOM, stop.

---

### Memory ownership

By default:

* input JSON buffers are owned by the caller or transport
* all extracted values are **spans** into those buffers
* copying is always explicit and opt-in

Rules:

* no implicit `malloc`
* no internal global allocators
* no long-lived heap state
* streaming buffers must be compacted, not reallocated repeatedly

If a function allocates, it must be obvious from the API.

---

### Error handling

Errors are **values**, not logs.

* every public API returns an error code
* error enums are stable and documented
* partial success is not allowed unless explicitly designed

Never swallow errors from:

* HTTP transport
* JSON parsing
* SSE framing
* protocol violations

---

## Layer Responsibilities

### Transport layer

Files:
* `llm_transport_*.c`

Responsibilities:

* HTTP request execution
* status code capture
* header handling
* streaming byte delivery

Must not:

* parse JSON
* interpret SSE
* retry implicitly
* mutate request bodies

The transport is dumb by design.

---

### JSON writer

Files:
* `llm_json_write.c`

Responsibilities:

* minimal append-only JSON emission
* correct string escaping
* deterministic output

Rules:

* no read-back
* no validation
* no pretty printing
* no dynamic structure inference

If the writer fails, it fails immediately.

---

### JSON reader

Files:
* `llm_json_read.c`

Responsibilities:

* token walking via `jstok`
* extracting known fields
* returning spans or typed values

Rules:

* helpers must hide token walking
* helpers must not allocate
* missing optional fields return “not found”, not errors
* missing required fields are errors

All helpers must be test-covered.

---

### Streaming (SSE)

Files:
* `llm_sse.c`

Responsibilities:

* buffering arbitrary byte chunks
* scanning complete `data:` frames
* handing JSON payloads to the parser

Rules:

* buffer must remain stable across partial parses
* `jstok_sse_next()` position must be respected
* compaction must preserve unread bytes
* `[DONE]` is a protocol signal, not JSON

No recursion in hot paths.

---

### Protocol semantics

Files:
* `llm_chat.c`
* `llm_completions.c`
* `llm_tools.c`

Responsibilities:

* request construction
* response interpretation
* tool loop execution
* finish_reason handling

Rules:

* never assume server correctness
* validate everything that matters
* keep state machines explicit
* tool loops must be bounded

Protocol logic belongs here and nowhere else.

---

## Tool Loop Rules

Tool execution must be:

* deterministic
* bounded
* observable

Rules:

* tools must have descriptions
* tool arguments must be valid JSON
* fragmented arguments must be accumulated verbatim
* loop detection must exist
* tool output is opaque to the client

Never auto-execute tools without caller involvement.

---

## MCP Server Rules

Files:
* `mcp_server.c`

The MCP server:

* wraps the same core APIs
* does not fork logic
* does not add parsing layers
* does not reinterpret responses

Rules:

* MCP messages map 1:1 to core calls
* errors propagate verbatim
* streaming remains streaming
* no convenience shortcuts

If MCP behavior differs from library behavior, that is a bug.

---

## Adding New Features

Before adding code, ask:

1. Is this protocol-required?
2. Can it be expressed as a thin helper?
3. Does it preserve zero-DOM parsing?
4. Does it avoid hidden allocation?
5. Can it be tested deterministically?

If the answer to any is “no”, stop.

---

## Testing Requirements

All new code must have tests in `tests/llmctl.c` or a new test file.

Tests must:

* parse results using `jstok`
* assert extracted spans and values
* cover streaming and non-stream paths
* validate tool behavior explicitly

Never test by string comparison alone.

---

## Debugging Tips

Recommended workflow:

* dump raw HTTP responses
* dump SSE frames before parsing
* log token arrays with indices
* assert token spans visually

Avoid printf-debugging inside tight loops.

---

## Style Guidelines

* C11
* explicit struct initialization
* no macros for control flow
* minimal preprocessor usage
* comments explain **why**, not **what**

If a comment explains what the code does, delete it.

---

## Final Rule

If you are unsure whether something belongs in `desi`, it probably does not.

Keep it small
Keep it strict
Keep it boring

That is the point

