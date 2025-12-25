# desi – Implementation TODO

This document tracks **remaining and follow-on work** to complete and harden `desi`, a minimal C LLM client and MCP server built on **`jstok` as a tokenizer/validator only**.

This TODO expands on **DESIGN.md**, **HACKING.md**, and **README.md** and should be read as an execution checklist.

If an item conflicts with architectural invariants, do not implement it.

---

## Current Focus (2025-12-25)

The library is functional end-to-end, but the remaining work is about:

* tightening contracts
* removing ambiguity
* increasing protocol coverage without adding abstraction
* making errors and limits mechanically enforceable
* hardening streaming and tool semantics under adversarial inputs

---

## 1) Transport, TLS, and Authentication

### Outcomes
* Transport remains a byte pump
* TLS and header knobs are explicit
* No hidden environment inspection
* No policy decisions in transport

### Tasks

- [x] **Transport contract document**
  - Write an internal `src/llm_transport_contract.h` comment block that defines:
    - who owns response body buffers and for how long
    - whether headers are stable during callbacks
    - streaming callback threading model
    - required failure propagation behavior
  - Add tests that assert contract behavior using a fake backend

- [ ] **TLS option surface audit**
  - Make every TLS knob explicit in public API
  - Minimum set:
    - CA bundle path
    - CA directory path if supported
    - hostname verification toggle (default on)
    - peer verification toggle (default on)
    - explicit insecure mode (must be loud and opt-in)
  - Ensure options never auto-load from env unless caller requests it

- [ ] **mTLS support**
  - Add explicit fields for:
    - client cert path
    - client key path
    - optional key password callback
  - Ensure:
    - no key material is copied into long-lived memory
    - paths are treated as config only
    - transport backend never logs secrets

- [ ] **API key and auth header rules**
  - Standardize auth handling:
    - bearer token support via caller-supplied string
    - per-request override without mutating client defaults
  - Add negative tests:
    - missing key yields expected server error parse
    - header injection does not corrupt framing

- [ ] **Proxy configuration**
  - Explicit proxy URL support
  - Explicit no-proxy list support
  - No auto-detection
  - Add tests that verify proxy config is passed through unchanged

---

## 2) Endpoint Coverage and Protocol Correctness

### Outcomes
* Coverage expands without duplication
* Pathing and endpoint naming are unambiguous
* Multi-choice semantics are explicit

### Tasks

- [ ] **Endpoint naming reconciliation**
  - Resolve `/props` vs `/health` mismatch
  - Choose one canonical endpoint name in code and tests
  - If compatibility is required, expose compatibility explicitly:
    - either a separate function
    - or a documented config knob
  - Never silently alias endpoints

- [ ] **Streaming completions**
  - Implement streamed completions using the same SSE engine as chat
  - Share extraction helpers
  - Ensure:
    - finish_reason handling matches chat
    - usage and model fields behave consistently
    - cancellation and limits apply identically

- [ ] **Multiple choices**
  - Define explicit API behavior for `n > 1`
  - Non-stream:
    - expose `choices[]` spans without allocation
    - provide helpers to index into `choices[i]`
  - Stream:
    - support parsing multiple streams by index only if caller enables it
    - default remains `choices[0]`
  - Add tests that validate:
    - choices ordering stability
    - missing indexes produce a stable error

- [ ] **Embeddings endpoint**
  - Request builder with explicit bounds:
    - input length caps
    - number-of-inputs caps
  - Response extraction:
    - `data[i].embedding` returned as spans or numeric parsing helper
  - No vector math, no allocator-heavy conversions by default

---

## 3) Streaming Hardening and Feature Expansion

### Outcomes
* SSE framing is hostile-input safe
* Fragmentation behavior is deterministic
* Tool deltas can be observed without changing aggregation mode

### Tasks

- [ ] **Strict line and frame caps**
  - Enforce:
    - `max_line_bytes`
    - `max_frame_bytes`
    - `max_sse_buffer_bytes`
  - Failure behavior:
    - return overflow error code
    - do not truncate silently
    - include the stage (SSE vs transport) in the error classification
  - Add tests for:
    - gigantic line without newline
    - repeated partial chunks
    - malicious never-ending frame

- [ ] **Tool delta callbacks**
  - Add optional callbacks that surface tool deltas while streaming:
    - tool_call index
    - id if present
    - name if present
    - raw argument fragment span
  - Preserve existing aggregation path
  - Ensure:
    - delta callback does not allocate
    - delta callback observes fragments exactly as received

- [ ] **Final tool aggregation callback**
  - When a tool call completes:
    - validate args as JSON using `jstok`
    - emit a single callback with the validated JSON span
  - Ensure:
    - completion detection is explicit and bounded
    - invalid final JSON fails with a protocol error

- [ ] **Cancellation**
  - Add a caller-provided abort hook checked at safe points:
    - after receiving bytes
    - after a completed SSE frame
    - between tool-loop turns
  - Cancellation must:
    - stop parsing cleanly
    - ask transport to abort
    - produce a stable cancellation error code
  - No signals, no threads

- [ ] **Streaming usage support**
  - Implement `include_usage` only as an explicit request option
  - Add tests for servers that do and do not include usage mid-stream

---

## 4) Tool Semantics and Tool Loop Extensions

### Outcomes
* Full modern tool protocol support
* Caller remains in control
* Loop remains bounded and observable

### Tasks

- [ ] **Outbound assistant tool_calls**
  - Allow constructing requests that include:
    - `role=assistant`
    - `tool_calls[]`
  - Provide two supported mechanisms:
    - an expanded typed struct
    - raw JSON injection for tool_calls only
  - Require:
    - deterministic serialization
    - explicit size caps

- [ ] **Tool loop parameter passthrough**
  - Allow caller to supply:
    - `params_json`
    - `response_format_json`
    - tool selection mode knobs if supported
  - No inference
  - No hidden defaults beyond the documented base defaults

- [ ] **Tool response helpers**
  - Provide helpers to create tool-result messages correctly:
    - attaches tool_call_id
    - attaches tool name if needed by server
  - Helpers must:
    - be thin
    - never execute tools
    - avoid allocation unless the caller provides a buffer

- [ ] **Tool loop safety**
  - Strengthen loop detection:
    - rolling hash of:
      - tool name
      - args JSON bytes
      - model message delta if relevant
  - Enforce:
    - max turns
    - max total tool args bytes per turn
    - max total output bytes appended to messages

---

## 5) Error Handling and Diagnostics

### Outcomes
* Errors are structured and inspectable
* Callers can surface precise failure causes without parsing strings
* No hidden global state

### Tasks

- [ ] **Unified structured error**
  - Define a stable `llm_error_detail_t` containing:
    - top-level error code enum
    - stage classification (transport, tls, sse, json, protocol)
    - http status if present
    - best-effort parsed OpenAI error fields
    - raw error body span when available
  - Ensure:
    - spans reference transport-owned buffers when possible
    - copying is explicit

- [ ] **OpenAI-style error parsing**
  - Best-effort extraction of:
    - `error.message`
    - `error.type`
    - `error.code`
  - Must not allocate
  - Must not fail the request if error body is malformed
  - Provide “not found” semantics for missing fields

- [ ] **Last error storage**
  - Provide one supported model:
    - per-request output struct preferred
  - If a “last error” accessor is desired:
    - make it per-client, not global
    - make it explicitly opt-in during client init
    - document thread-safety rules
  - Never use hidden global state

- [ ] **Error stringification helper**
  - Provide `llm_errstr(code)` for stable, boring text
  - No dynamic formatting

---

## 6) Request Options and Feature Coverage

### Outcomes
* Common knobs are available without turning requests into a DSL
* Raw injection exists only for narrow escape hatches
* Every option remains explicitly bounded

### Tasks

- [ ] **Request options struct**
  - Define `llm_request_opts_t` with optional fields:
    - temperature
    - top_p
    - max_tokens
    - stop (array or single string)
    - frequency_penalty
    - presence_penalty
    - seed
  - All fields are optional
  - Serialization is deterministic
  - Bounds:
    - max stop strings
    - max stop bytes
    - reject NaNs and infinities

- [ ] **Multi-part content**
  - Support `content[]` messages without media decoding
  - Provide either:
    - typed support for common parts
    - or narrow raw injection for `content` only
  - Enforce:
    - part count cap
    - total content bytes cap

- [ ] **Model switching**
  - Provide `llm_client_set_model()` or equivalent
  - Must not invalidate in-flight requests
  - Must not reallocate hidden client internals
  - Caller manages synchronization

- [ ] **Reasoning fields and variants**
  - Add extraction helpers for vendor variants if needed:
    - `reasoning_content`
    - separate “thinking” channels if present
  - These must remain spans with explicit opt-in parsing

---

## 7) Tests, Fuzzing, and CI Hardening

### Outcomes
* Regression prevention without brittle fixtures
* Hostile inputs covered
* Streaming edge cases locked down

### Tasks

- [ ] **Unit test expansion**
  - finish_reason variants
  - tool_calls parsing across fragmented frames
  - missing fields vs optional fields semantics
  - multi-choice indexing behavior

- [ ] **SSE edge-case tests**
  - `[DONE]` behaviors
  - malformed frames
  - line overflow
  - partial UTF-8 boundaries
  - repeated empty `data:` lines
  - frames containing non-JSON payloads

- [ ] **Fake transport backend**
  - Create a deterministic in-memory transport for tests
  - Must support:
    - streaming chunk schedules
    - injected HTTP status codes
    - injected headers
    - controlled buffer lifetimes
  - This removes reliance on live servers for correctness testing

- [ ] **Integration test gating**
  - All live integration tests must be opt-in
  - Require explicit env var or CLI flag
  - Skip cleanly when server unreachable

- [ ] **Fuzz surfaces**
  - SSE scanner
  - JSON extraction helpers
  - tool args accumulator
  - Ensure fuzz harnesses are deterministic and bounded

---

## 8) Examples and Operational Patterns

### Outcomes
* Examples teach correct usage
* No convenience shortcuts that violate invariants
* Configuration patterns stay explicit

### Tasks

- [ ] **Environment-driven config example**
  - Demonstrate explicit reading of:
    - `LLM_BASE_URL`
    - `LLM_API_KEY`
  - Document no-auth local usage
  - Ensure the library itself does not auto-read env

- [ ] **Streaming tool example**
  - Demonstrates:
    - fragmented args
    - delta callbacks
    - final aggregation
    - bounded tool loop

- [ ] **mTLS example**
  - Demonstrate:
    - CA override
    - client cert + key
    - strict verification on
  - Keep it minimal and explicit

- [ ] **MCP server example**
  - Minimal invocation
  - No protocol sugar
  - Error mapping demo

---

## 9) Documentation and Release Polish

### Outcomes
* Project is easy to audit and embed
* Public API is stable and documented
* Memory and limit behavior is validated

### Tasks

- [ ] **Public API documentation**
  - Add Doxygen comments in `llm.h`
  - Focus on:
    - ownership rules
    - lifetime rules
    - error codes
    - limit behavior
  - Avoid duplicating prose across docs

- [ ] **pkg-config correctness**
  - Validate clean static and shared builds
  - Ensure dependencies are correct and minimal
  - Add a tiny compile test that uses only `pkg-config --cflags --libs`

- [ ] **Memory validation**
  - ASan/UBSan builds in CI
  - Valgrind run for non-stream tests
  - Streaming stress tests with fake transport

- [ ] **Versioning**
  - Set initial version
  - Tag release
  - Keep changelog minimal and factual

---

## Execution Rules

This TODO favors:

* explicitness over coverage
* correctness over convenience
* boring code over clever code

If a task introduces hidden allocation, implicit behavior, duplicated protocol logic, or cross-layer leakage, reject or redesign it.

Keep `desi` small  
Keep it strict  
Keep it predictable
