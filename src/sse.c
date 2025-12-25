#include "llm/internal.h"
#define JSTOK_HEADER
#include <jstok.h>
#include <stdlib.h>
#include <string.h>

typedef struct sse_parser sse_parser_t;

struct sse_parser {
    char* buf;        // accumulated buffer
    size_t buf_size;  // allocated size
    size_t buf_len;   // used length
    size_t max_line_bytes;
    size_t max_total_bytes;
    size_t total_bytes_seen;
    void* user_data;
    void (*on_data_line)(void* user_data, span_t line);
    bool is_done;
};

sse_parser_t* sse_create(size_t max_line_bytes, size_t max_total_bytes) {
    sse_parser_t* parser = malloc(sizeof(*parser));
    if (!parser) return NULL;
    memset(parser, 0, sizeof(*parser));
    parser->max_line_bytes = max_line_bytes;
    parser->max_total_bytes = max_total_bytes;
    return parser;
}

void sse_destroy(sse_parser_t* parser) {
    if (parser) {
        free(parser->buf);
        free(parser);
    }
}

void sse_set_callback(sse_parser_t* parser, void (*cb)(void* user_data, span_t line), void* user_data) {
    parser->on_data_line = cb;
    parser->user_data = user_data;
}

bool sse_is_done(sse_parser_t* parser) { return parser->is_done; }

static int ensure_capacity(sse_parser_t* parser, size_t extra) {
    size_t needed = parser->buf_len + extra;
    if (needed > parser->buf_size) {
        size_t new_size = parser->buf_size ? parser->buf_size * 2 : 4096;
        if (new_size < needed) new_size = needed;
        char* new_buf = realloc(parser->buf, new_size);
        if (!new_buf) return -1;
        parser->buf = new_buf;
        parser->buf_size = new_size;
    }
    return 0;
}

int sse_feed(sse_parser_t* parser, const char* chunk, size_t chunk_len) {
    if (chunk_len == 0 || parser->is_done) return 0;
    if (ensure_capacity(parser, chunk_len) < 0) return -1;
    memcpy(parser->buf + parser->buf_len, chunk, chunk_len);
    parser->buf_len += chunk_len;
    parser->total_bytes_seen += chunk_len;
    if (parser->max_total_bytes && parser->total_bytes_seen > parser->max_total_bytes) {
        return -1;  // exceeded cap
    }
    // Process lines
    int pos = 0;
    while (pos < (int)parser->buf_len) {
        jstok_span_t jspan;
        int ret = jstok_sse_next(parser->buf, (int)parser->buf_len, &pos, &jspan);
        if (ret == 1) {
            // data line found
            if (jspan.n == 6 && memcmp(jspan.p, "[DONE]", 6) == 0) {
                parser->is_done = true;
                break;
            }
            span_t line = {jspan.p, jspan.n};
            if (parser->on_data_line) {
                parser->on_data_line(parser->user_data, line);
            }
            // continue processing
        } else if (ret == 0) {
            // need more data
            break;
        } else {
            // error
            return -1;
        }
    }
    // Shift remaining data to front
    if (pos > 0) {
        size_t remaining = parser->buf_len - pos;
        if (remaining > 0) {
            memmove(parser->buf, parser->buf + pos, remaining);
        }
        parser->buf_len = remaining;
    }
    return 0;
}