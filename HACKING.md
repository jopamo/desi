# HACKING.md

This document describes **how to work on `desi` correctly**.

If behavior in code diverges from what is described here, **the code is wrong**.

---

## What `desi` Is

`desi` is a **strict, low-level HTTP + TLS client and protocol binding layer** for talking to LLM-style APIs (and similar structured HTTP services).

In the larger system:

```

ET (identity, authorization, audit, policy)
└── uses desi for outbound HTTP(S) calls
├── llama-server (stateless inference appliance)
├── registries
├── internal APIs
└── MCP servers

desi = transport + parsing + protocol semantics

```

`desi` exists so higher layers can be correct without re-implementing:

* TLS verification and client cert handling
* bounded streaming
* deterministic JSON emission
* zero-DOM JSON extraction
* explicit protocol state machines

`desi` is intentionally small, explicit, and hostile to abstraction creep.

---

## What `desi` Is Not

`desi` must never become:

* an authorization system
* a device identity manager
* a task executor
* a retry/orchestration engine
* a cache
* a scheduler
* an agent framework
* a convenience wrapper that hides failure

Those belong elsewhere:

* **ET** handles identity, authorization, audit, and fleet policy
* **llama-server** handles inference only
* **callers** decide intent, retries, and policy

If a feature needs cross-request memory, implicit behavior, or policy interpretation, it does not belong in `desi`.

---

## Core Philosophy

Design principles:

* **No JSON DOM**
* **No implicit allocation**
* **No hidden protocol behavior**
* **No background threads**
* **No magic retries**
* **No guessing**

Every byte, token, and buffer must be explainable.

---

## Trust and PKI Boundary

`desi` participates in **authentication**, not **authorization**.

Rules:

* TLS verification is always on by default
* the system CA store is used by default
* mTLS (client certs) is supported only by explicit caller configuration
* `desi` never decides who is allowed to call what

`desi` may enforce mechanical constraints:

* size limits
* timeouts
* scheme restrictions (https)
* redirect limits
* strict parsing and framing rules

But it must not encode policy decisions.

---

## Architectural Invariants

These rules are non-negotiable.

### Zero-DOM JSON

`desi` does not build a JSON object graph.

* tokenization and extraction only
* spans into caller-owned buffers
* explicit copying only

If you find yourself wanting a DOM, stop.

---

### `jstok` Usage Rules

`jstok` is used only for:

* tokenization
* validation
* path-based extraction
* SSE `data:` frame scanning

`jstok` must never be used for:

* request construction
* object ownership
* protocol decisions
* schema enforcement beyond local validation
* caching or memoization

`jstok` is a tokenizer, not a runtime.

---

### Memory Ownership

By default:

* input JSON buffers are owned by the caller or transport
* all extracted values are **spans** into those buffers
* copying is always explicit and opt-in

Rules:

* no implicit `malloc`
* no internal global allocators
* no long-lived heap state
* streaming buffers must be compacted, not reallocated repeatedly

If a function allocates, it must be obvious from the API name and signature.

---

### Error Handling

Errors are values, not logs.

Rules:

* every public API returns an error code
* error enums are stable and documented
* partial success is not allowed unless explicitly designed
* errors must preserve the stage of failure (transport vs parse vs protocol)

Never swallow errors from:

* HTTP transport
* TLS verification
* JSON parsing
* SSE framing
* protocol violations

---

## Layer Responsibilities

`desi` is a set of tight layers with sharp boundaries.

### Transport Layer

Files:

* `llm_transport_*.c`

Responsibilities:

* HTTP request execution
* status code capture
* header handling
* streaming byte delivery
* TLS verification and client cert plumbing (as configured)

Must not:

* parse JSON
* interpret SSE
* retry implicitly
* mutate request bodies
* infer content types or semantics

The transport is dumb by design.

---

### JSON Writer

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

### JSON Reader

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
* bounds checks are mandatory

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
* SSE parsing is framing only, not semantics

No recursion in hot paths.

---

### Protocol Semantics

Files:

* `llm_chat.c`
* `llm_completions.c`
* `llm_tools.c`

Responsibilities:

* request construction
* response interpretation
* tool loop execution
* finish_reason handling
* explicit state machines and validation

Rules:

* never assume server correctness
* validate everything that matters
* keep state machines explicit
* tool loops must be bounded
* treat all remote strings as untrusted input

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
* the caller must opt in to running tools

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

1. Is this protocol-required or required for correctness
2. Can it be expressed as a thin helper without adding hidden behavior
3. Does it preserve zero-DOM parsing
4. Does it avoid hidden allocation
5. Does it preserve layer boundaries
6. Can it be tested deterministically

If the answer to any is “no”, stop.

---

## Testing Requirements

All new code must have tests in `tests/llmctl.c` or a new test file.

Tests must:

* parse results using `jstok`
* assert extracted spans and typed values
* cover streaming and non-stream paths
* validate tool behavior explicitly
* cover failure modes and error codes

Never test by string comparison alone.

---

## Debugging Tips

Recommended workflow:

* dump raw HTTP responses (headers + body length)
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
* comments explain why, not what

If a comment explains what the code does, delete it.

---

## Final Rule

If you are unsure whether something belongs in `desi`, it probably does not.

Keep it small  
Keep it strict  
Keep it boring  

That is the point
