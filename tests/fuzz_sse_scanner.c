#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sse.h"

enum { FUZZ_MAX_INPUT = 4096 };

struct sse_fuzz_state {
    size_t lines_seen;
    size_t frames_seen;
    size_t bytes_seen;
    size_t max_line;
};

static bool on_event(void* user_data, const sse_event_t* event) {
    struct sse_fuzz_state* st = user_data;
    st->lines_seen++;
    st->bytes_seen += event->data.len;
    if (event->data.len > st->max_line) st->max_line = event->data.len;
    return true;
}

static bool on_frame(void* user_data) {
    struct sse_fuzz_state* st = user_data;
    st->frames_seen++;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data) return 0;
    if (size > FUZZ_MAX_INPUT) size = FUZZ_MAX_INPUT;
    if (size == 0) return 0;

    uint8_t cfg0 = data[0];
    uint8_t cfg1 = (size > 1) ? data[1] : 0;
    uint8_t cfg2 = (size > 2) ? data[2] : 0;
    uint8_t cfg3 = (size > 3) ? data[3] : 0;

    size_t max_line = 1 + (cfg0 % 128);
    size_t max_frame = 1 + (cfg1 % 256);
    size_t max_buf = 1 + (cfg2 % 512);
    size_t max_total = (cfg3 & 1) ? 0 : (size_t)(1 + (cfg3 % 256));

    sse_parser_t* parser = sse_create(max_line, max_frame, max_buf, max_total);
    if (!parser) return 0;

    struct sse_fuzz_state st = {0};
    sse_set_callback(parser, on_event, &st);
    sse_set_frame_callback(parser, on_frame, &st);

    const uint8_t* payload = data;
    size_t payload_len = size;
    size_t pos = 0;

    while (pos < payload_len) {
        size_t chunk = 1 + (pos % 16);
        if (chunk > payload_len - pos) chunk = payload_len - pos;
        int rc = sse_feed(parser, (const char*)payload + pos, chunk);
        if (rc != SSE_OK) break;
        pos += chunk;
    }

    sse_destroy(parser);
    return 0;
}
