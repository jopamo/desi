# desi – Remaining Implementation TODO

This document tracks **unfinished and follow-on work** required to complete and harden `desi`, a minimal C LLM client and MCP server built on **`jstok` strictly as a tokenizer/validator**.

This TODO expands on **DESIGN.md**, **HACKING.md**, and **README.md**.
It is an **execution checklist**, not a roadmap.

If a task violates architectural invariants, **do not implement it**.

---

## Current Focus (2025-12-25)

The core library is functional end-to-end.

Remaining work focuses on:

* tightening contracts
* eliminating ambiguity
* hardening streaming and tool semantics under hostile input
* enforcing limits mechanically
* removing reliance on live backends for correctness testing

---

## 1) Streaming Hardening and Completion Semantics

### Outcomes

* SSE parsing is hostile-input safe
* Completion boundaries are explicit and verifiable
* Streaming remains allocation-free by default

### Tasks

* **SSE edge-case coverage** (done)

  Add tests for:

  * `[DONE]` handling
  * malformed frames
  * line overflow
  * partial UTF-8 boundaries
  * repeated empty `data:` lines
  * non-JSON payload frames

  Parsing must never:

  * overrun buffers
  * assume line atomicity
  * infer completion implicitly

---

## 2) Tool Semantics and Loop Safety

### Outcomes

* Tool loops are bounded, observable, and auditable
* No hidden inference or automatic retries
* Failure modes are deterministic

### Tasks

*No remaining implementation tasks in this section.*

Tool semantics are considered complete pending test coverage and fuzzing.

---

## 3) Error Handling and Diagnostics

### Outcomes

* Errors are structured, inspectable, and stable
* No string parsing required by callers
* No hidden global state

### Tasks

*No remaining implementation tasks in this section.*

Validation depends on fuzzing and fake transport coverage below.

---

## 4) Request Options and Feature Coverage

### Outcomes

* Common knobs are explicit and bounded
* Raw injection exists only as a narrow escape hatch
* No request DSL creep

### Tasks

*No remaining implementation tasks in this section.*

---

## 5) Tests, Fuzzing, and CI Hardening

### Outcomes

* Streaming regressions are prevented
* Hostile inputs are covered
* CI failures are meaningful and localizable

### Tasks

* **Fake transport backend** (done)

  Implement a deterministic in-memory transport for tests that supports:

  * scheduled streaming chunks
  * injected HTTP status codes
  * injected headers
  * controlled buffer lifetimes

  Requirements:

  * zero networking
  * fully synchronous and deterministic
  * reusable across unit, integration, and fuzz tests

* **Fuzz surfaces**

  Add bounded, deterministic fuzz harnesses for:

  * SSE scanner
  * JSON span extraction helpers
  * tool-argument accumulator

  Fuzzing must:

  * enforce hard input size limits
  * avoid unbounded loops
  * never depend on wall-clock time

---

## 6) Examples and Operational Patterns

### Outcomes

* Examples teach correct usage
* No shortcuts that violate invariants
* Configuration remains explicit and auditable

### Tasks

* **Environment-driven configuration example**

  Demonstrate explicit reading of:

  * `LLM_BASE_URL`
  * `LLM_API_KEY`

  Document:

  * unauthenticated local usage
  * expected failure modes when unset

  The library itself must **never** read environment variables.

* **Streaming tool example**

  Demonstrate:

  * fragmented tool arguments
  * delta callbacks
  * final aggregation callback
  * bounded tool loop termination

* **mTLS example**

  Demonstrate:

  * CA override
  * client certificate + key
  * strict verification enabled

  Keep minimal and explicit.

* **MCP server example**

  Demonstrate:

  * minimal invocation
  * explicit request/response mapping
  * error translation without protocol sugar

---

## 7) Documentation and Release Polish

### Outcomes

* API is auditable
* Embedding behavior is predictable
* Limits and lifetimes are explicit

### Tasks

* **Public API documentation**

  Add Doxygen comments to `llm.h` covering:

  * ownership rules
  * lifetime guarantees
  * error models
  * limit enforcement behavior

  Avoid duplicating prose from other documents.

* **pkg-config correctness**

  Validate static and shared builds.

  Requirements:

  * minimal, accurate dependency list
  * successful compile using only:

    ```
    pkg-config --cflags --libs desi
    ```

* **Memory validation**

  Add CI coverage for:

  * ASan and UBSan builds
  * Valgrind runs for non-stream paths
  * streaming stress tests using the fake transport

* **Versioning**

  * set initial version
  * tag release
  * keep changelog minimal and factual

---

## Execution Rules

This TODO favors:

* explicitness over coverage
* correctness over convenience
* boring code over clever code

If a task introduces hidden allocation, implicit behavior, duplicated protocol logic, or cross-layer leakage, **reject it or redesign it**.

Keep `desi` small
Keep it strict
Keep it predictable
