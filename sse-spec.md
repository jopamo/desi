# **Server-Sent Events (SSE) — Architectural Truth**

> **Authority:** Based on [HTML Living Standard][1].
> **MIME Type:** `text/event-stream`

## **1. Transport & Headers**

The stream is delivered over HTTP. The underlying TCP connection must be held open.

### **HTTP/1.1**

* **Headers:**
```http
HTTP/1.1 200 OK
Content-Type: text/event-stream; charset=utf-8
Cache-Control: no-store
Connection: keep-alive

```



### **HTTP/2**

* **Headers:**
```http
:status: 200
content-type: text/event-stream; charset=utf-8
cache-control: no-store

```


* **Constraint:** The `Connection` header **MUST NOT** be present in HTTP/2.

### **Caching & Proxies**

* Servers SHOULD send `Cache-Control: no-store` to prevent intermediaries from buffering the stream.
* If `no-cache` is used, legacy proxies may still buffer. `no-store` is the safer default for real-time control streams.

### **Reconnection**

* Clients **SHOULD** automatically attempt to reconnect if the connection drops.
* **Backoff:** Clients SHOULD respect the delay set by the last received `retry:` field.
* **Resumption:** On reconnect, clients MUST send the `Last-Event-ID` header if an `id:` was previously received.

---

## **2. Framing & Encoding**

### **Text Encoding**

* Stream MUST be **UTF-8**.
* **Safety:** Invalid UTF-8 sequences **MUST NOT** cause crashes or buffer overruns. They MAY be replaced (e.g., `U+FFFD`) or treated as parse errors, but behavior must be deterministic.
* **BOM:** If the stream starts with `U+FEFF`, it MUST be stripped.

### **Line Terminators**

The stream is a sequence of lines. A line is terminated by:

1. `\r\n` (CRLF)
2. `\n` (LF)
3. `\r` (CR)

*Implementation Note:* A parser MUST treat `\r\n` as a single terminator, not two.

---

## **3. Field Parsing Rules**

Each line is parsed as a field.
Format: `Field: Value`

1. **Split:** Locate the *first* colon.
* **Left:** Field Name.
* **Right:** Field Value.


2. **Trim:** If the *first* character of the Value is a space (`U+0020`), remove it. (Remove exactly one space. Do not trim trailing whitespace).
3. **No Colon:** If no colon exists, the whole line is the Field Name, and Value is an empty string.

---

## **4. Event State Machine**

The parser maintains four state variables:

1. **`data_buffer`** (append-only string)
2. **`event_type_buffer`** (string)
3. **`last_event_id`** (string, **persists** across events and reconnects)
4. **`retry_time`** (integer, **persists**, never dispatched)

### **Field Processing**

| Field | Action |
| --- | --- |
| `data` | Append `<value>` + `\n` to `data_buffer`. Flag that data exists. |
| `event` | Overwrite `event_type_buffer` with `<value>`. |
| `id` | Overwrite `last_event_id`. **Ignore** if `<value>` contains NUL (`\0`). |
| `retry` | Parse digits. Update `retry_time`. **Ignore** if non-digits. |
| *Comment* | (Line starts with `:`) Ignore completely. |
| *Other* | Ignore completely. |

### **Dispatch Logic (On Blank Line)**

When a blank line is encountered:

1. **The "Empty Data" Check:**
* Has the `data` field been seen for this event block?
* **NO:** Abort. Do not dispatch. Clear buffers. (Even if `event:` or `id:` were set).
* **YES:** Proceed.
* *Note:* `data:` (empty value) counts as "YES". It produces a buffer containing `\n`.


2. **Trailing Newline Trim:**
* If `data_buffer` ends with `\n`, remove the *last* `\n`.


3. **Dispatch Event:**
* **Type:** `event_type_buffer` (default: `"message"`).
* **ID:** `last_event_id` (current value).
* **Payload:** `data_buffer`.


4. **Reset:**
* Clear `data_buffer`.
* Clear `event_type_buffer`.
* Reset "data seen" flag.
* (*Do NOT clear `last_event_id` or `retry_time*`).



---

## **Artifact: Single-Pass C State Machine Outline**

This outlines the precise logic for a C11 parser (compatible with your `ET` or `netcat` architectures). It avoids allocations for line splitting by processing character-by-character.

```c
typedef struct {
    // Persistent State
    char *last_event_id;
    uint32_t retry_ms;

    // Current Event Buffers
    char *data_buf;       // Growable
    size_t data_len;
    char *event_type;     // Growable
  
    // Parser State flags
    bool data_seen;       // Did we see a 'data:' field in this block?
    bool is_cr;           // Was previous char '\r'? (Handle CRLF)
} sse_parser_t;

void sse_parse_chunk(sse_parser_t *ctx, const char *chunk, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = chunk[i];

        // 1. Handle Line Endings (CRLF, LF, CR) normalization
        if (c == '\n' || c == '\r') {
            if (ctx->is_cr && c == '\n') {
                ctx->is_cr = false;
                continue; // Skip the LF part of CRLF
            }
            ctx->is_cr = (c == '\r');
  
            // End of Line Detected: Process the Line
            process_line(ctx); // See logic below
            clear_line_buffer(ctx);
        } else {
            ctx->is_cr = false;
            append_char_to_line(ctx, c);
        }
    }
}

// Logic triggered on EOL
void process_line(sse_parser_t *ctx) {
    if (line_is_empty(ctx)) {
        // --- DISPATCH STAGE ---
        if (ctx->data_seen) {
            // Trim final newline
            if (ctx->data_len > 0 && ctx->data_buf[ctx->data_len - 1] == '\n') {
                 ctx->data_buf[ctx->data_len - 1] = '\0';
                 ctx->data_len--;
            }
  
            // Dispatch
            emit_event(
                ctx->event_type ? ctx->event_type : "message",
                ctx->data_buf,
                ctx->last_event_id
            );
        }
  
        // Reset Event Buffers (but NOT persistent state)
        reset_event_buffers(ctx);
        ctx->data_seen = false;
        return;
    }

    // --- FIELD PARSING STAGE ---
    // 1. Find first colon
    char *colon = strchr(ctx->line_buf, ':');
    char *val;
  
    if (colon) {
        *colon = '\0'; // Split
        val = colon + 1;
        if (*val == ' ') val++; // Strip ONE leading space
    } else {
        val = ""; // Empty value
    }

    // 2. Route Field
    if (strcmp(ctx->line_buf, "data") == 0) {
        append_data(ctx, val);
        append_data(ctx, "\n");
        ctx->data_seen = true;
    }
    else if (strcmp(ctx->line_buf, "event") == 0) {
        set_event_type(ctx, val);
    }
    else if (strcmp(ctx->line_buf, "id") == 0) {
        if (!strchr(val, '\0')) { // Spec: ignore if null inside
            set_last_event_id(ctx, val);
        }
    }
    else if (strcmp(ctx->line_buf, "retry") == 0) {
        if (is_digits_only(val)) {
            ctx->retry_ms = atoi(val);
        }
    }
    // else: ignore comments and unknown fields
}
