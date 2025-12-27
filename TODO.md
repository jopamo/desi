# desi – Remaining Implementation TODO

This document tracks **unfinished and follow-on work** required to complete and harden `desi`.

`desi` is now a **single repository** that ships:

* `libdesi` (strict HTTP/TLS + SSE framing + zero-DOM JSON + protocol bindings)
* `desid` (the only LLM gateway service)
* `agentd` (LAN orchestrator + SSE event stream)
* `mcpd` (HTTP tool servers)

This TODO expands on **DESIGN.md**, **HACKING.md**, and **README.md**
It is an **execution checklist**, not a roadmap

If a task violates architectural invariants, **do not implement it**

---

## Current Focus (2025-12-26)

Phase 1 goal:

* run agents on a LAN with no auth
* keep the LLM boundary strict (only desid reaches backends)
* make SSE correctness mechanical and testable everywhere
* make orchestration deterministic and bounded
* remove reliance on live backends for correctness testing

---

## 0) Repo Restructure and Build Outputs

### Outcomes

* Clear separation between `libdesi`, `desid`, `agentd`, `mcpd`
* Binaries build reproducibly with minimal deps
* Tests do not depend on network

### Tasks

* **Define canonical targets**
  * `libdesi` builds as static + shared
  * `desid` builds as a standalone HTTP server
  * `agentd` builds as a standalone HTTP+SSE server
  * `mcpd` builds as standalone tool servers or one toolbox server

* **Repo layout normalization**
  * move daemon-specific code out of `src/lib/`
  * ensure shared utilities live in a dedicated internal module
  * avoid circular dependencies between daemons and libdesi

* **Install and packaging**
  * install headers for `libdesi`
  * install `desid`, `agentd`, `mcpd-*` binaries
  * verify `pkg-config --cflags --libs desi` works for embedding libdesi

---

## 1) SSE Core: Compliance, Reuse, and Tests

### Outcomes

* One SSE parser and one SSE writer used everywhere
* Spec-compliant framing with hostile-input safety
* No duplicate ad-hoc SSE code in daemons

### Tasks

* **SSE parser compliance**
  * implement UTF-8 decode behavior with one leading BOM strip
  * accept CRLF, LF, and CR line endings
  * comment lines beginning with `:` are ignored
  * field parsing matches the spec (`event`, `data`, `id`, `retry`)
  * dispatch only on blank line
  * discard incomplete trailing event at EOF

* **SSE writer**
  * deterministic emission of `event:` and `data:` blocks
  * optional comment keepalive emission (`: ping`)
  * enforce max event size and max line size

* **SSE test matrix**
  * line ending variations (CRLF/LF/CR)
  * leading BOM cases
  * empty data dispatch suppression behavior
  * `data:` multi-line concatenation and trailing LF removal
  * `id` containing NUL is ignored
  * `retry` digits-only update behavior
  * huge lines and truncation behavior
  * partial chunk boundaries across every delimiter
  * compaction correctness (pos and unread bytes)

* **SSE fuzz surfaces**
  * bounded fuzz input for the parser
  * bounded fuzz input for writer round-trip properties
  * ensure no quadratic behavior on repeated small chunks

---

## 2) `desid`: LLM Gateway Service

### Outcomes

* `desid` is the only component that talks to LLM backends
* Streaming is preserved end-to-end
* Mechanical limits are enforced consistently and tested

### Tasks

* **HTTP surface definition**
  * OpenAI-compatible endpoints
  * document request/response schemas and error codes

* **LLM boundary enforcement**
  * no LLM backend URLs or creds in `agentd` or `mcpd`
  * tests assert `agentd` never attempts direct LLM connections
  * integration tests enforce topology assumptions via fake transport

* **Streaming proxy correctness**
  * propagate streaming responses as SSE without buffering whole body
  * ensure SSE framing is valid and not interleaved
  * define termination behavior for `[DONE]` at the protocol layer

* **Limit enforcement**
  * max request bytes (incoming)
  * max response bytes (outgoing)
  * max streaming bytes
  * header size limits
  * redirect limits
  * timeouts (connect, first byte, total)

* **desid tests**
  * non-stream and stream paths using fake transport
  * injected upstream protocol errors produce stable downstream errors
  * backpressure behavior does not grow buffers unbounded

---

## 3) `agentd`: Orchestrator and Run Engine

### Outcomes

* Runs are deterministic, bounded, and observable
* Tool loops are policy-controlled at the orchestrator
* Clients can attach via SSE and reconstruct progress from the stream alone

### Tasks

* **Run model**
  * run lifecycle: create, status, cancel
  * bounded counters: max steps, max tool calls, max wallclock
  * explicit step types:
    * LLM step: call `desid`
    * tool step: call `mcpd`
    * finalize

* **Event stream**
  * `GET /v1/runs/{id}/events` streams SSE events
  * define canonical event types:
    * run.started
    * step.started
    * llm.delta
    * llm.completed
    * tool.requested
    * tool.completed
    * run.completed
    * run.failed
    * run.cancelled
  * ensure ordering and deterministic payload shapes

* **Tool loop policy**
  * per-run allowlist of tools
  * loop detection and hard max iterations
  * tool execution must be explicitly enabled
  * tool output treated as opaque data

* **Failure semantics**
  * cancellation behavior is immediate and observable
  * timeouts become explicit error events
  * partial progress is preserved as events but run ends in failed state

* **agentd tests**
  * deterministic replay tests using fake `desid` + fake `mcpd`
  * step bound enforcement tests
  * cancellation and timeout tests
  * SSE event stream compliance tests (writer reuse)

---

## 4) `mcpd`: Tool Server Framework and Built-in Tools

### Outcomes

* Tool servers are deterministic, bounded, and hostile-input safe
* Tool servers never talk to LLM backends
* Tool servers expose a stable HTTP surface usable by agentd

### Tasks

* **HTTP contract**
  * define the tool RPC surface
    * MCP JSON-RPC over HTTP, or
    * a strict desi-native tool call API
  * document tool listing, tool call, and error payload shapes

* **Tool framework**
  * registry of tools (name, description, JSON schema)
  * input validation is mechanical and bounded
  * output size caps and timeouts

* **Wikipedia tool**
  * search by query/title
  * fetch summary or extract
  * cap response sizes and strip unneeded fields
  * deterministic outputs

* **mcpd tests**
  * schema validation tests
  * hostile input tests (huge args, invalid JSON, slow upstream)
  * deterministic output tests
  * strict limits enforcement tests

---

## 5) Fake Backends and Test Harness Expansion

### Outcomes

* All correctness is testable without networking
* Streaming regressions are prevented
* Hostile inputs are covered with fuzzing

### Tasks

* **Fake HTTP transport backend**
  * scheduled streaming chunks
  * injected HTTP statuses and headers
  * controlled buffer lifetimes
  * deterministic and synchronous

* **Fake desid server for agentd tests**
  * scripted responses for chat/completions
  * scripted streaming SSE sequences
  * scripted tool-call emissions

* **Fake mcpd server for agentd tests**
  * scripted tool results
  * injected tool failures and timeouts

* **Fuzz surfaces**
  * SSE parser and writer
  * JSON extraction helpers
  * tool-argument accumulator
  * orchestrator event stream ingestion and emission

---

## 6) Hardening: Limits, Timeouts, and Backpressure

### Outcomes

* No unbounded memory growth under hostile input
* Behavior is defined at every limit boundary
* Backpressure does not corrupt streams

### Tasks

* **Uniform limits table**
  * define one set of defaults used by libdesi + daemons
  * document which limits are per-request vs per-process

* **Timeout model**
  * connect timeout
  * first-byte timeout
  * total request timeout
  * streaming idle timeout (if any)

* **Backpressure tests**
  * slow client reading SSE from agentd
  * slow client reading SSE from desid
  * slow upstream sending tiny chunks forever
  * ensure buffers cap and fail explicitly

---

## 7) Documentation and Release Polish

### Outcomes

* Docs match reality and enforce boundaries
* Public APIs are auditable
* The suite is runnable by a new contributor

### Tasks

* **Docs updates**
  * README.md: suite overview and process topology
  * HACKING.md: boundary rules and invariants for daemons
  * DESIGN.md: include agentd and mcpd structures and SSE compliance rules
  * add a short RUNNING.md for LAN no-auth mode

* **Public API documentation**
  * Doxygen for public headers
  * ownership rules, lifetimes, error surfaces, limits

* **Operational examples**
  * minimal LAN setup:
    * agentd exposed on LAN
    * desid and mcpd bound to localhost
    * llama-server bound to localhost
  * streaming run event demo
  * wikipedia tool demo

* **CI hardening**
  * ASan + UBSan builds
  * valgrind runs for non-stream paths
  * streaming stress tests using fakes
  * fuzz job with strict input size caps

* **Versioning**
  * set initial version
  * tag release
  * changelog is factual and minimal

---

## Execution Rules

This TODO favors:

* explicitness over convenience
* correctness over features
* boring code over clever code

If a task introduces hidden allocation, implicit behavior, duplicated protocol logic, cross-layer leakage, or violates the LLM boundary, **reject it or redesign it**

Keep libdesi small  
Keep desid boring  
Keep agentd explicit  
Keep mcpd deterministic
