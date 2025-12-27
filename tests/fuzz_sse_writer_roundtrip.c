#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sse.h"

enum { FUZZ_MAX_INPUT = 2048, FUZZ_MAX_OUT = 16384 };

struct fuzz_cursor {
    const uint8_t* data;
    size_t len;
    size_t pos;
};

static uint8_t take_u8(struct fuzz_cursor* cur) {
    if (cur->pos >= cur->len) return 0;
    return cur->data[cur->pos++];
}

static size_t take_span(struct fuzz_cursor* cur, const uint8_t** out) {
    if (cur->pos >= cur->len) {
        *out = NULL;
        return 0;
    }
    size_t remaining = cur->len - cur->pos;
    size_t want = take_u8(cur);
    size_t len = remaining ? (want % (remaining + 1)) : 0;
    *out = cur->data + cur->pos;
    cur->pos += len;
    return len;
}

struct sse_roundtrip {
    span_t data;
    span_t event_type;
    size_t events;
};

static bool on_event(void* user_data, const sse_event_t* event) {
    struct sse_roundtrip* cap = user_data;
    cap->events++;
    if (cap->events == 1) {
        cap->data = event->data;
        cap->event_type = event->event_type;
    }
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data) return 0;
    if (size > FUZZ_MAX_INPUT) size = FUZZ_MAX_INPUT;
    if (size < 4) return 0;

    struct fuzz_cursor cur = {.data = data, .len = size, .pos = 0};
    size_t max_line = 1 + (take_u8(&cur) % 128);
    size_t max_frame = 1 + (take_u8(&cur) % 512);
    size_t out_cap = 1 + (take_u8(&cur) % FUZZ_MAX_OUT);

    const uint8_t* event_src = NULL;
    size_t event_len = take_span(&cur, &event_src);
    const uint8_t* data_src = cur.data + cur.pos;
    size_t data_len = cur.len - cur.pos;

    char event_buf[FUZZ_MAX_INPUT];
    char data_buf[FUZZ_MAX_INPUT];

    if (event_len > 0) {
        for (size_t i = 0; i < event_len; i++) {
            event_buf[i] = (char)(event_src[i] & 0x7F);
        }
    }
    for (size_t i = 0; i < data_len; i++) {
        data_buf[i] = (char)(data_src[i] & 0x7F);
    }

    sse_write_limits_t limits = {.max_line_bytes = max_line, .max_frame_bytes = max_frame};
    char out[FUZZ_MAX_OUT];
    size_t out_len = 0;
    int rc = sse_write_event(&limits, event_buf, event_len, data_buf, data_len, out, out_cap, &out_len);
    if (rc != SSE_OK) return 0;

    sse_parser_t* parser = sse_create(max_line, max_frame, 0, 0);
    if (!parser) return 0;

    struct sse_roundtrip cap = {0};
    sse_set_callback(parser, on_event, &cap);

    size_t pos = 0;
    while (pos < out_len) {
        size_t chunk = 1 + (pos % 7);
        if (chunk > out_len - pos) chunk = out_len - pos;
        rc = sse_feed(parser, out + pos, chunk);
        if (rc != SSE_OK) {
            sse_destroy(parser);
            return 0;
        }
        pos += chunk;
    }

    if (cap.events != 1) __builtin_trap();
    if (cap.data.len != data_len) __builtin_trap();
    if (data_len > 0 && memcmp(cap.data.ptr, data_buf, data_len) != 0) __builtin_trap();

    if (event_len > 0) {
        if (cap.event_type.len != event_len) __builtin_trap();
        if (event_len > 0 && memcmp(cap.event_type.ptr, event_buf, event_len) != 0) __builtin_trap();
    } else {
        if (cap.event_type.len != strlen("message")) __builtin_trap();
        if (memcmp(cap.event_type.ptr, "message", cap.event_type.len) != 0) __builtin_trap();
    }

    sse_destroy(parser);
    return 0;
}
