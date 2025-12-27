# HACKING.md

This document describes **how to work on `desi` correctly**.

If behavior in code diverges from what is described here, **the code is wrong**.

---

## What `desi` Is

`desi` is a **strict, low-level C foundation for LAN agent systems**, centered on:

* **`libdesi`**: HTTP(S) client + TLS/mTLS plumbing + SSE framing + zero-DOM JSON + LLM protocol bindings
* **`desid`**: an HTTP gateway daemon that talks to LLM backends using `libdesi`
* **`agentd`**: an orchestration daemon that runs bounded agent state machines and streams run events
* **`mcpd`**: tool server daemons that expose explicit capabilities over HTTP

In the larger system, **ET** can later front these, but it is not required for correctness inside this repo.

---

## The LLM Boundary

This is the most important architectural invariant in the repository

**Only `desid` is permitted to connect to LLM backends**

* `llama-server` and other LLM endpoints must not be reachable directly by `agentd`, `mcpd`, or LAN clients
* `agentd` obtains model outputs by calling `desid`
* `mcpd` implements tools and must never call an LLM

This is enforced by deployment topology and must remain true in code as well

---

## What `desi` Is Not

`desi` must never become:

* an authorization system
* a device identity manager
* a general scheduler
* a cache
* a convenience wrapper that hides failure
* a DOM-based JSON runtime
* a “do everything” agent framework inside `libdesi`

Separation of concerns:

* `desid` is the only LLM gateway
* `agentd` orchestrates runs and tool loops
* `mcpd` provides tools
* ET (when present) owns identity, authorization, audit, and fleet policy

If a feature needs implicit behavior, cross-request memory, or policy interpretation, it does not belong in `libdesi`

---

## Core Philosophy

Design principles:

* **No JSON DOM**
* **No implicit allocation**
* **No hidden protocol behavior**
* **No background threads**
* **No magic retries**
* **No guessing**

Every byte, token, and buffer must be explainable

---

## Trust and PKI Boundary

`libdesi` and `desid` participate in **authentication**, not **authorization**

Rules:

* TLS verification is always on by default
* the system CA store is used by default
* mTLS is supported only by explicit configuration
* neither `libdesi` nor `desid` decides who is allowed to call what

`libdesi` and `desid` may enforce mechanical constraints:

* size limits
* timeouts
* scheme restrictions (https)
* redirect limits
* strict parsing and framing rules

But they must not encode policy decisions

---

## Architectural Invariants

These rules are non-negotiable

### Zero-DOM JSON

`libdesi` does not build a JSON object graph

* tokenization and extraction only
* spans into caller-owned buffers
* explicit copying only

If you find yourself wanting a DOM, stop

---

### `jstok` Usage Rules

`jstok` is used only for:

* tokenization
* validation
* path-based extraction
* validating JSON fragments (including streamed tool argument fragments)
* scanning SSE payload boundaries when needed

`jstok` must never be used for:

* request construction
* object ownership
* protocol decisions
* schema enforcement beyond local validation
* caching or memoization

`jstok` is a tokenizer, not a runtime

---

### Memory Ownership

By default:

* input buffers are owned by the caller or request context
* extracted values are spans into those buffers
* copying is explicit and opt-in

Rules:

* no implicit `malloc`
* no internal global allocators
* no long-lived heap state in hot paths
* streaming buffers must be compacted, not reallocated repeatedly

If a function allocates, it must be obvious from the API name and signature

---

### Error Handling

Errors are values, not logs

Rules:

* every public API returns an error code
* error enums are stable and documented
* errors preserve the failing stage (transport vs tls vs sse vs parse vs protocol)
* partial success is not allowed unless explicitly designed

Never swallow errors from:

* HTTP transport
* TLS verification
* JSON parsing
* SSE framing
* protocol violations

---

## Layer Responsibilities

`libdesi` is a set of tight layers with sharp boundaries

### Transport Layer

Files:

* `llm_transport_*.c` (or `desi_transport_*.c`)

Responsibilities:

* HTTP request execution
* status code capture
* header handling
* streaming byte delivery
* TLS verification and client cert plumbing (as configured)

Must not:

* parse JSON
* interpret SSE semantics
* retry implicitly
* mutate request bodies
* infer content types or protocol meaning

The transport is a byte pump by design

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

If the writer fails, it fails immediately

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

All helpers must be test-covered

---

### Streaming (SSE)

Files:

* `llm_sse.c`

Responsibilities:

* buffering arbitrary byte chunks
* parsing SSE framing into events
* yielding complete `data` payloads to higher layers

Rules:

* SSE parsing is framing only, not semantics
* buffer must remain stable across partial parses
* compaction must preserve unread bytes
* no recursion in hot paths

SSE framing must follow the EventSource processing rules:

* UTF-8 decode, strip one leading BOM if present
* accept CRLF, LF, or CR line endings
* ignore comment lines beginning with `:`
* handle `event`, `data`, `id`, `retry` fields as specified
* dispatch only on blank lines
* discard incomplete trailing events at EOF

Do not implement ad-hoc SSE parsers outside the SSE module

---

### LLM Protocol Bindings

Files:

* `llm_chat.c`
* `llm_completions.c`
* `llm_tools.c`

Responsibilities:

* request construction
* response interpretation
* explicit state machines and validation
* assembling fragmented tool-call arguments when the upstream streams them

Rules:

* never assume server correctness
* validate everything that matters
* keep state machines explicit
* treat all remote strings as untrusted input

`libdesi` may decode and assemble tool-call arguments, but it must not decide tool policy or execute tools

---

## Daemon Responsibilities

The daemons are thin and must not fork the core library logic

### `desid` (LLM gateway)

Responsibilities:

* expose an HTTP API for LLM calls
* call `libdesi` for outbound LLM HTTP(S)
* preserve streaming end-to-end
* enforce mechanical limits (timeouts, byte caps, redirect caps)

Must not:

* execute tool loops
* implement orchestration policy
* reinterpret tool output
* hide failures

If `desid` behavior differs from `libdesi` semantics, that is a bug

---

### `agentd` (orchestrator)

Responsibilities:

* run lifecycle (create, status, cancel)
* explicit bounded step machine
* tool coordination and loop limits
* run event stream as SSE

Rules:

* tool execution is caller-controlled and bounded
* loop detection must exist
* all steps must be observable as events
* orchestration must be deterministic given the same inputs

Must not:

* talk to LLM backends directly
* embed LLM credentials or backend addresses
* hide failures through implicit retries

---

### `mcpd` (tool servers)

Responsibilities:

* expose tools as explicit capabilities over HTTP
* validate and bound inputs
* return deterministic, size-limited outputs

Rules:

* tools never talk to LLM backends
* tool servers must be hostile-input safe
* no unbounded caches
* no implicit orchestration behavior

---

## Tool Loop Rules

Tool execution must be:

* deterministic
* bounded
* observable

Policy lives in `agentd`

Rules:

* tools must have descriptions
* tool arguments must be valid JSON
* fragmented arguments must be accumulated verbatim
* loop detection and maximum iterations must exist
* tool output is opaque data to the model
* tool execution must be explicitly enabled per run

Never auto-execute tools inside `desid` or `libdesi`

---

## Adding New Features

Before adding code, ask:

1. Is this required for correctness or protocol compliance
2. Can it be expressed as a thin helper without hidden behavior
3. Does it preserve zero-DOM parsing
4. Does it avoid hidden allocation
5. Does it preserve layer boundaries
6. Can it be tested deterministically

If the answer to any is “no”, stop

---

## Testing Requirements

All new code must have tests

Tests must:

* parse results using `jstok`
* assert extracted spans and typed values
* cover streaming and non-stream paths
* validate tool behavior explicitly
* cover failure modes and error codes
* exercise hard limits and hostile inputs

Never test by string comparison alone

---

## Debugging Tips

Recommended workflow:

* dump raw HTTP responses (headers + body length)
* dump SSE frames before higher-level parsing
* log token arrays with indices
* assert token spans visually

Avoid printf-debugging inside tight loops

---

## Style Guidelines

* C11
* explicit struct initialization
* no macros for control flow
* minimal preprocessor usage
* comments explain why, not what

If a comment explains what the code does, delete it

---

## Final Rule

If you are unsure whether something belongs in `libdesi`, it probably does not

Keep libdesi small  
Keep desid boring  
Keep agentd explicit  
Keep mcpd deterministic  

That is the point
