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

static void on_data_line(void* user_data, span_t line) {
    struct sse_fuzz_state* st = user_data;
    st->lines_seen++;
    st->bytes_seen += line.len;
    if (line.len > st->max_line) st->max_line = line.len;
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
    sse_set_callback(parser, on_data_line, &st);
    sse_set_frame_callback(parser, on_frame, &st);

    size_t pos = 0;
    while (pos < size) {
        size_t chunk = 1 + (data[pos] % 16);
        if (chunk > size - pos) chunk = size - pos;
        int rc = sse_feed(parser, (const char*)data + pos, chunk);
        if (rc != SSE_OK || sse_is_done(parser)) break;
        pos += chunk;
    }

    sse_destroy(parser);
    return 0;
}
