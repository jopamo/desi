#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sse.h"

enum { FUZZ_MAX_INPUT = 8192 };

static void on_data_line(void* user_data, span_t line) {
    (void)user_data;
    (void)line;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data) return 0;
    if (size > FUZZ_MAX_INPUT) size = FUZZ_MAX_INPUT;
    if (size < 4) return 0;

    uint8_t cfg0 = data[0];
    uint8_t cfg1 = data[1];
    uint8_t cfg2 = data[2];
    uint8_t cfg3 = data[3];

    size_t max_line = (cfg0 & 0x80) ? 0 : (size_t)(1 + (cfg0 % 64));
    size_t max_frame = (cfg1 & 0x80) ? 0 : (size_t)(1 + (cfg1 % 64));
    size_t max_buf = (cfg2 & 0x80) ? 0 : (size_t)(1 + (cfg2 % 128));
    size_t max_total = (cfg3 & 0x80) ? 0 : (size_t)(1 + (cfg3 % 256));

    sse_parser_t* parser = sse_create(max_line, max_frame, max_buf, max_total);
    if (!parser) return 0;
    sse_set_callback(parser, on_data_line, NULL);

    const char* payload = (const char*)data + 4;
    size_t payload_len = size - 4;
    sse_feed(parser, payload, payload_len);

    sse_destroy(parser);
    return 0;
}
