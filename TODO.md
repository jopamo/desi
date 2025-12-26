Here’s a **cleaned and tightened TODO** with **all completed items removed**, scope sharpened, and wording adjusted to better reflect execution order and invariants. Nothing new is added, only clarification and de-duplication.

---

# desi – Remaining Implementation TODO

This document tracks **unfinished and follow-on work** to complete and harden `desi`, a minimal C LLM client and MCP server built on **`jstok` strictly as a tokenizer/validator**.

This TODO expands on **DESIGN.md**, **HACKING.md**, and **README.md** and is an execution checklist, not a wish list.

If a task violates architectural invariants, do not implement it.

---

## Current Focus (2025-12-25)

The core library is functional end-to-end.

Remaining work is about:

* tightening contracts
* eliminating ambiguity
* extending protocol coverage without abstraction creep
* making limits mechanically enforceable
* hardening streaming and tool semantics under hostile inputs

---

## 1) Streaming Hardening and Completion Semantics

### Outcomes

* SSE parsing is hostile-input safe
* Completion boundaries are explicit
* Streaming remains allocation-free by default

### Tasks

* [x] **Final tool aggregation callback**

  * When a tool call completes:

    * validate arguments as JSON using `jstok`
    * emit a single callback with the validated JSON span
  * Completion detection must be:

    * explicit
    * bounded
    * independent of transport chunking
  * Invalid final JSON must fail with a protocol error

* [x] **Cancellation**

  * Add a caller-provided abort hook checked at safe points:

    * after receiving bytes
    * after a completed SSE frame
    * between tool-loop turns
  * Cancellation must:

    * stop parsing cleanly
    * instruct transport to abort
    * return a stable cancellation error code
  * No signals
    No threads

* [x] **Streaming usage support**

  * Support `include_usage` only as an explicit request option
  * Add tests for:

    * servers that emit usage mid-stream
    * servers that emit usage only at completion
    * servers that omit usage entirely

---

## 2) Tool Semantics and Tool Loop Extensions

### Outcomes

* Modern tool protocol fully supported
* Caller remains in control
* Tool loops are bounded and observable

### Tasks

* [x] **Outbound assistant tool_calls**

  * Allow constructing assistant messages containing `tool_calls[]`
  * Provide exactly two mechanisms:

    * a typed, structured builder
    * raw JSON injection for `tool_calls` only
  * Require:

    * deterministic serialization
    * explicit size caps

* [x] **Tool loop parameter passthrough**

  * Allow caller to supply:

    * `params_json`
    * `response_format_json`
    * tool selection mode knobs if supported by backend
  * No inference
    No silent defaults beyond documented base defaults

* [ ] **Tool response helpers**

  * Provide helpers to construct tool-result messages:

    * attaches `tool_call_id`
    * attaches tool name if required by server
  * Helpers must:

    * be thin
    * never execute tools
    * avoid allocation unless caller provides a buffer

* [ ] **Tool loop safety**

  * Strengthen loop detection using a rolling hash of:

    * tool name
    * argument JSON bytes
    * relevant model deltas if applicable
  * Enforce:

    * max tool turns
    * max total tool-argument bytes per turn
    * max total output bytes appended to messages

---

## 3) Error Handling and Diagnostics

### Outcomes

* Errors are structured and inspectable
* No string parsing required by callers
* No hidden global state

### Tasks

* [ ] **Unified structured error**

  * Define `llm_error_detail_t` containing:

    * top-level error code enum
    * stage classification (transport, tls, sse, json, protocol)
    * HTTP status if present
    * best-effort parsed OpenAI-style error fields
    * raw error body span when available
  * Spans must reference transport-owned buffers where possible
  * Copying must always be explicit

* [ ] **OpenAI-style error parsing**

  * Best-effort extraction of:

    * `error.message`
    * `error.type`
    * `error.code`
  * Must not allocate
  * Must not fail the request if error body is malformed
  * Missing fields yield explicit “not present” semantics

* [ ] **Last error storage**

  * Preferred model:

    * per-request output struct
  * If a “last error” accessor exists:

    * make it per-client
    * require explicit opt-in at client init
    * document thread-safety rules
  * Never use global state

* [ ] **Error stringification helper**

  * Provide `llm_errstr(code)`
  * Output must be:

    * stable
    * boring
    * non-formatted

---

## 4) Request Options and Feature Coverage

### Outcomes

* Common knobs are available without turning requests into a DSL
* Raw injection exists only as a narrow escape hatch
* Every option is explicitly bounded

### Tasks

* [ ] **Request options struct**

  * Define `llm_request_opts_t` with optional fields:

    * temperature
    * top_p
    * max_tokens
    * stop (single or array)
    * frequency_penalty
    * presence_penalty
    * seed
  * Serialization must be deterministic
  * Enforce bounds:

    * max stop strings
    * max stop bytes
    * reject NaNs and infinities

* [ ] **Multi-part content**

  * Support `content[]` messages without media decoding
  * Provide either:

    * typed helpers for common parts
    * or raw JSON injection limited to `content`
  * Enforce:

    * max part count
    * max total content bytes

* [ ] **Model switching**

  * Provide `llm_client_set_model()` or equivalent
  * Must not:

    * invalidate in-flight requests
    * reallocate hidden client internals
  * Caller owns synchronization

* [ ] **Reasoning fields and vendor variants**

  * Add extraction helpers for optional vendor fields:

    * `reasoning_content`
    * separate “thinking” channels if present
  * Parsing must remain:

    * opt-in
    * span-based
    * allocation-free by default

---

## 5) Tests, Fuzzing, and CI Hardening

### Outcomes

* Streaming regressions are prevented
* Hostile inputs are covered
* CI failures are meaningful

### Tasks

* [ ] **Unit test expansion**

  * finish_reason variants
  * fragmented tool_calls across frames
  * optional vs missing field semantics
  * multi-choice indexing failures

* [ ] **SSE edge-case tests**

  * `[DONE]` handling
  * malformed frames
  * line overflow
  * partial UTF-8 boundaries
  * repeated empty `data:` lines
  * non-JSON payload frames

* [ ] **Fake transport backend**

  * Deterministic in-memory transport for tests
  * Must support:

    * scheduled streaming chunks
    * injected HTTP status codes
    * injected headers
    * controlled buffer lifetimes
  * Removes reliance on live servers

* [ ] **Integration test gating**

  * All live tests must be opt-in
  * Require explicit env var or CLI flag
  * Skip cleanly when server is unreachable

* [ ] **Fuzz surfaces**

  * SSE scanner
  * JSON extraction helpers
  * tool-argument accumulator
  * Harnesses must be deterministic and bounded

---

## 6) Examples and Operational Patterns

### Outcomes

* Examples teach correct usage
* No shortcuts that violate invariants
* Configuration remains explicit

### Tasks

* [ ] **Environment-driven config example**

  * Demonstrate explicit reading of:

    * `LLM_BASE_URL`
    * `LLM_API_KEY`
  * Document unauthenticated local usage
  * Library itself must never auto-read env

* [ ] **Streaming tool example**

  * Demonstrate:

    * fragmented arguments
    * delta callbacks
    * final aggregation callback
    * bounded tool loop

* [ ] **mTLS example**

  * Demonstrate:

    * CA override
    * client cert + key
    * strict verification enabled
  * Keep minimal and explicit

* [ ] **MCP server example**

  * Minimal invocation
  * No protocol sugar
  * Explicit error mapping

---

## 7) Documentation and Release Polish

### Outcomes

* API is auditable
* Embedding is predictable
* Limits and lifetimes are explicit

### Tasks

* [ ] **Public API documentation**

  * Add Doxygen comments to `llm.h`
  * Focus on:

    * ownership rules
    * lifetime guarantees
    * error models
    * limit behavior
  * Avoid duplicating prose across documents

* [ ] **pkg-config correctness**

  * Validate static and shared builds
  * Dependencies must be minimal and accurate
  * Add a tiny compile test using only:

    * `pkg-config --cflags --libs`

* [ ] **Memory validation**

  * ASan and UBSan builds in CI
  * Valgrind runs for non-stream paths
  * Streaming stress tests using fake transport

* [ ] **Versioning**

  * Set initial version
  * Tag release
  * Keep changelog minimal and factual

---

## Execution Rules

This TODO favors:

* explicitness over coverage
* correctness over convenience
* boring code over clever code

If a task introduces hidden allocation, implicit behavior, duplicated protocol logic, or cross-layer leakage, reject it or redesign it.

Keep `desi` small
Keep it strict
Keep it predictable
