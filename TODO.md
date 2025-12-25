# desi – Implementation TODO

This document tracks **remaining and follow-on work** to complete and harden `desi`, a minimal C LLM client and MCP server built on **`jstok` as a tokenizer/validator only**.

This TODO expands on **DESIGN.md**, **HACKING.md**, and **README.md** and should be read as an execution checklist, not a wish list.

If an item conflicts with architectural invariants, **do not implement it**.

---

## Current Status (2025-12-25)

### ✅ Implemented and Verified

**Project & Build**
- Project layout (`include/`, `src/`, `tests/`, `examples/`)
- Meson build system with:
  - libcurl transport
  - vendored `jstok`
- Clean separation of public vs internal headers

**Core Library**
- Public API (`include/llm/llm.h`)
- Deterministic buffer ownership rules
- No JSON DOM, no hidden allocation

**JSON Handling**
- Minimal JSON request writer (`json_build.c`)
- JSON core helpers (`json_core.c/.h`)
- Typed extraction helpers over `jstok` tokens

**Protocol Coverage**
- Chat completions (non-stream)
- Streaming chat (SSE)
- Completions endpoint (non-stream)
- Tool call parsing and accumulation
- Tool loop runner
- response_format (`json_object`, `json_schema`) validation

**Streaming**
- Stateful SSE buffer with caps
- `[DONE]` detection
- Fragmented tool argument reassembly

**Transport**
- libcurl backend
- Streaming callbacks
- Size caps and failure propagation

**Tests & Examples**
- `tests/llmctl.c` matching `llm_power` expectations
- Examples:
  - simple chat
  - streaming chat
  - tool loop

---

## Remaining Implementation Work

The sections below expand each area with **design-aligned constraints**.

---

## 1) Transport & Authentication

### Goals
Expose necessary knobs without bloating the transport layer or leaking protocol logic into it.

### Tasks

- [x] **API key support**
  - Support `Authorization: Bearer …`
  - Never store secrets globally
  - Allow per-client default key

- [ ] **Custom headers**
  - Organization / project headers
  - Arbitrary user-provided headers
  - Headers are copied at client init, not per request

- [ ] **Per-request headers**
  - Required for proxies, multi-tenant gateways
  - Must not mutate client defaults

- [ ] **TLS configuration**
  - CA bundle path
  - Optional insecure mode (explicit flag only)
  - No implicit environment inspection

- [ ] **Proxy support**
  - Explicit proxy URL
  - No auto-detection logic

**Non-goals**
- OAuth
- Token refresh
- Header mutation during retries

---

## 2) Endpoint Coverage & Correctness

### Goals
Match real-world API surface without special cases.

### Tasks

- [ ] **Fix `/props` vs `/health`**
  - Align implementation and tests
  - Document exact semantics
  - No silent aliasing

- [ ] **Streaming completions**
  - Use existing SSE infrastructure
  - Same extraction rules as chat streaming
  - No duplicated code paths

- [ ] **Multiple choices support**
  - Non-stream:
    - expose `choices[]` as spans
  - Stream:
    - allow caller to opt into multi-choice parsing
  - Default behavior remains `choices[0]`

- [ ] **Embeddings endpoint**
  - Request builder only
  - Response parsing via jstok spans
  - No vector math helpers

---

## 3) Streaming Enhancements

### Goals
Make streaming robust without turning it into a framework.

### Tasks

- [ ] **Strict line caps**
  - Enforce `max_line_bytes` in SSE feed
  - Fail fast on overflow
  - Do not truncate silently

- [ ] **Tool delta callbacks**
  - Expose:
    - tool_call index
    - optional id
    - optional name
    - argument fragment
  - Preserve existing aggregated mode

- [ ] **Final tool aggregation callback**
  - Called once per completed tool call
  - Provides validated JSON span

- [ ] **Cancellation support**
  - Caller-provided abort hook
  - Must stop transport and parsing cleanly
  - No async signals or threads

- [ ] **Optional stream options**
  - `include_usage`
  - Implement only if explicitly requested

---

## 4) Tooling & Tool Loop Extensions

### Goals
Support advanced tool flows without hiding control.

### Tasks

- [ ] **Outbound assistant tool_calls**
  - Allow request messages with:
    - `role=assistant`
    - embedded `tool_calls`
  - Either via expanded message struct or raw JSON injection

- [ ] **Tool loop parameter passthrough**
  - `params_json`
  - `response_format_json`
  - Must be caller-controlled

- [ ] **Tool response helpers**
  - Auto-attach:
    - `tool_call_id`
    - tool name
  - Avoid duplication in user code

**Non-goals**
- Automatic tool execution
- Parallel tool dispatch

---

## 5) Error Handling & Diagnostics

### Goals
Errors must be inspectable, structured, and boring.

### Tasks

- [ ] **Structured error result**
  - HTTP status
  - Raw error body span
  - Transport vs parse vs protocol classification

- [ ] **OpenAI-style error parsing**
  - Extract:
    - `error.message`
    - `error.type`
    - `error.code`
  - Optional, best-effort

- [ ] **`llm_last_error()` accessor**
  - Thread-local or per-client
  - Read-only snapshot
  - No hidden global state

---

## 6) Request Schema & Feature Coverage

### Goals
Expose common knobs without turning requests into DSLs.

### Tasks

- [ ] **Multi-part content**
  - Support `content[]` arrays
  - Or allow raw JSON injection for messages
  - No media decoding

- [ ] **Request options struct**
  - temperature
  - top_p
  - max_tokens
  - stop
  - frequency/presence penalties
  - Always optional

- [ ] **Model switching**
  - `llm_client_set_model()`
  - Must not invalidate in-flight requests
  - No reallocation of client internals

---

## 7) Tests & CI Hardening

### Goals
Prevent regressions without fragile fixtures.

### Tasks

- [ ] **Focused unit tests**
  - finish_reason variants
  - tool_calls parsing
  - reasoning_content handling

- [ ] **SSE edge-case tests**
  - `[DONE]`
  - malformed frames
  - line overflow
  - partial UTF-8 boundaries

- [ ] **Integration test gating**
  - Skip when server unreachable
  - Explicit opt-in via env var

- [ ] **Fuzz surfaces**
  - JSON reader helpers
  - SSE scanner

---

## 8) Examples & Usage

### Goals
Show correct patterns, not convenience shortcuts.

### Tasks

- [ ] **Environment-driven config**
  - `LLM_BASE_URL`
  - `LLM_API_KEY`
  - Document no-auth local usage

- [ ] **Streaming tool example**
  - Demonstrates:
    - fragmented args
    - aggregation
    - tool loop completion

- [ ] **MCP server example**
  - Minimal invocation
  - No protocol sugar

---

## 9) Documentation & Release Polish

### Goals
Make the project easy to audit and embed.

### Tasks

- [ ] **API documentation**
  - Doxygen comments in `llm.h`
  - No prose duplication

- [ ] **pkg-config verification**
  - Ensure clean static and shared builds

- [ ] **Memory validation**
  - Valgrind
  - ASan/UBSan
  - Streaming stress tests

- [ ] **Versioning**
  - Set initial version
  - Tag release
  - Changelog is optional

---

## Final Notes

This TODO intentionally favors:

* explicitness over coverage
* correctness over convenience
* boring code over clever code

If a task introduces:
- hidden allocation
- implicit behavior
- duplicated protocol logic

it should be rejected or redesigned.

Keep `desi` small  
Keep it strict  
Keep it predictable
