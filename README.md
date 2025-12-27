<div style="background-color:#1e1e1e; padding:1em; display:inline-block; border-radius:8px; text-align:left;">
  <img src=".github/desi.png" alt="desi logo" width="300" style="display:block; margin:0;">
</div>

`desi` is a **deterministic C LLM gateway + LAN orchestration suite**, built on **`jstok`** and a strict **SSE-first** streaming model.

It is designed for systems programmers who want:

* predictable parsing (no JSON DOM)
* explicit protocol state machines
* mechanically bounded streaming
* simple, inspectable daemons that compose over HTTP

`desi` is one repository that ships:

* **`libdesi`**: strict HTTP(S) client + SSE framing + zero-DOM JSON + LLM protocol bindings
* **`desid`**: HTTP gateway to LLM backends (**the only service allowed to talk to LLMs**)
* **`agentd`**: LAN orchestrator (runs, step loops, tool coordination, run event stream)
* **`mcpd`**: HTTP tool servers (Wikipedia, internal APIs, registries, etc)

---

## Where `desi` Fits

`desi` is the “boring correctness” layer and the minimal LAN control plane that sits near inference.

Long-term:

```

ET (identity, authorization, audit, policy)
└── fronts agentd/desid
├── agentd (runs + tool loops)
├── desid  (LLM gateway)
└── mcpd-* (tool servers)
|
└── libdesi (transport + SSE + jstok parsing)

```

Phase 1 (no auth, LAN-only):

```

LAN clients ──HTTP──> agentd
├──HTTP──> desid ──HTTP──> llama-server / other LLM backends
└──HTTP──> mcpd-* ──HTTP──> external/internal APIs

```

**Hard invariant:** only `desid` reaches LLM backends. Everyone else talks to `desid`.

---

## Goals

### Suite goals

* **HTTP-first**
  Everything composes over HTTP on a LAN

* **Streaming-first**
  SSE is the primitive for progress, deltas, and long-lived flows

* **Deterministic orchestration**
  `agentd` runs explicit, bounded state machines with observable steps

* **No hidden behavior**
  No magic retries, no implicit caching, no background scheduler threads

### `libdesi` goals

* **Zero JSON DOM**
  `jstok` tokenization + validation + span extraction only

* **Protocol honest**
  Explicit state machines for LLM semantics and streaming behavior

* **Mechanically bounded**
  Hard limits for request/response/stream buffers and tool argument assembly

* **Embeddable**
  No global state, no hidden allocators, no background threads

---

## Non-goals

### Across the suite

* No authorization decisions (ET or callers own policy)
* No “agent intelligence” baked into tool servers
* No silent retries or inference of intent

### `libdesi` specifically

* No generic JSON object model
* No orchestration framework
* No caching layer
* No scheduling engine

If you need those, they belong in `agentd` or ET, not in the library core.

---

## Architecture Overview

### Component view

```

┌─────────────┐     HTTP      ┌─────────────┐     HTTP      ┌──────────────┐
│  LAN client  │ ───────────> │   agentd    │ ───────────> │    desid      │
└─────────────┘    (SSE)      └─────────────┘    (SSE)      └──────┬───────┘
│                                   │
│ HTTP                               │ HTTP
v                                   v
┌──────────┐                        ┌────────────┐
│  mcpd-*  │                        │ llama-server│
└────┬─────┘                        └────────────┘
│
v
external APIs

All outbound HTTP client behavior comes from libdesi

```

### Library layering (inside libdesi)

```

┌───────────────────────────┐
│ Protocol bindings         │  chat / tools / streaming semantics
└───────────┬───────────────┘
│
┌───────────▼───────────────┐
│ JSON read/write           │  tiny writer + jstok extractors, spans only
└───────────┬───────────────┘
│
┌───────────▼───────────────┐
│ SSE core                  │  framing + event parsing, no semantics
└───────────┬───────────────┘
│
┌───────────▼───────────────┐
│ Transport                 │  HTTP(S) byte pump + TLS/mTLS plumbing
└───────────────────────────┘

```

Layer rules:

* transport is a byte pump
* SSE does framing only
* JSON does tokenization/extraction only
* protocol owns semantics and validation

No layer mutates data owned by another layer.

---

## SSE Compliance

`desi` treats SSE as a real protocol with strict parsing rules:

* UTF-8 decode; strip exactly one leading BOM if present
* line endings may be CRLF, LF, or CR
* `data:` appends value + LF to the data buffer
* blank line dispatches one event and resets data/event-type buffers
* `id:` updates last-event-id (unless it contains NUL)
* `retry:` updates reconnection time when digits-only
* comment lines starting with `:` are ignored

SSE parsing is framing only; higher layers decide what `data` means.

---

## Tool Servers (`mcpd-*`)

Tool servers are standalone daemons that expose explicit capabilities over HTTP.

* tools never talk to LLMs
* tool outputs are opaque data to callers
* tool execution is always caller-controlled and bounded by `agentd`

Each `mcpd-*` is expected to be deterministic, size-bounded, and hostile-input safe.

---

## Repository Layout

```

include/
desi/                 public headers (libdesi)

src/
lib/                  libdesi core
desi_transport_*.c
desi_json_write.c
desi_json_read.c
desi_sse.c
desi_chat.c
desi_tools.c

desid/                 LLM gateway daemon (HTTP server)
agentd/                orchestration daemon (HTTP + SSE server)
mcpd/                  tool server framework + built-in tools
wikipedia/
...

third_party/
jstok/                 vendored tokenizer

tests/
libdesi_*.c            transport/SSE/JSON/protocol tests
agentd_*.c             run-state-machine and limits tests
mcpd_*.c               tool behavior + hostile input tests

examples/
client_llm.c
client_sse.c
client_agentd.c

````

(Exact filenames may vary, but the boundary is fixed: lib core vs daemons.)

---

## Building

```sh
meson setup build
meson compile -C build
````

---

## Testing

```sh
meson test -C build
```

All tests validate behavior by:

* parsing with `jstok`
* asserting spans and typed extracts
* exercising streaming and reconnection behavior
* covering failure paths and hard limits

---

## License

MIT
