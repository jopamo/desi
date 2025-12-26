#include "sse.h"

#include <stdlib.h>
#include <string.h>

typedef struct sse_parser sse_parser_t;

struct sse_parser {
    char* buf;        // accumulated buffer
    size_t buf_size;  // allocated size
    size_t buf_len;   // used length
    size_t max_line_bytes;
    size_t max_frame_bytes;
    size_t max_sse_buffer_bytes;
    size_t max_total_bytes;
    size_t total_bytes_seen;
    size_t frame_bytes;
    void* user_data;
    void (*on_data_line)(void* user_data, span_t line);
    void* frame_user_data;
    sse_frame_cb on_frame;
    bool is_done;
    int last_error;
};

sse_parser_t* sse_create(size_t max_line_bytes, size_t max_frame_bytes, size_t max_sse_buffer_bytes,
                         size_t max_total_bytes) {
    sse_parser_t* parser = malloc(sizeof(*parser));
    if (!parser) return NULL;
    memset(parser, 0, sizeof(*parser));
    parser->max_line_bytes = max_line_bytes;
    parser->max_frame_bytes = max_frame_bytes;
    parser->max_sse_buffer_bytes = max_sse_buffer_bytes;
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

void sse_set_frame_callback(sse_parser_t* parser, sse_frame_cb cb, void* user_data) {
    parser->on_frame = cb;
    parser->frame_user_data = user_data;
}

bool sse_is_done(sse_parser_t* parser) { return parser->is_done; }

static int ensure_capacity(sse_parser_t* parser, size_t extra) {
    if (extra == 0) return SSE_OK;
    size_t needed = parser->buf_len + extra;
    if (parser->max_sse_buffer_bytes && needed > parser->max_sse_buffer_bytes) {
        return SSE_ERR_OVERFLOW_BUFFER;
    }
    if (needed > parser->buf_size) {
        size_t new_size = parser->buf_size ? parser->buf_size * 2 : 4096;
        if (new_size < needed) new_size = needed;
        if (parser->max_sse_buffer_bytes && new_size > parser->max_sse_buffer_bytes) {
            new_size = parser->max_sse_buffer_bytes;
        }
        if (new_size < needed) return SSE_ERR_OVERFLOW_BUFFER;
        char* new_buf = realloc(parser->buf, new_size);
        if (!new_buf) return SSE_ERR_NOMEM;
        parser->buf = new_buf;
        parser->buf_size = new_size;
    }
    return SSE_OK;
}

static int sse_set_error(sse_parser_t* parser, int err) {
    if (parser && parser->last_error == SSE_OK) {
        parser->last_error = err;
    }
    return err;
}

int sse_feed(sse_parser_t* parser, const char* chunk, size_t chunk_len) {
    if (!parser) return SSE_ERR_NOMEM;
    if (parser->last_error != SSE_OK) return parser->last_error;
    if (chunk_len == 0 || parser->is_done) return SSE_OK;

    if (parser->max_total_bytes) {
        size_t remaining = parser->max_total_bytes - parser->total_bytes_seen;
        if (chunk_len > remaining) {
            return sse_set_error(parser, SSE_ERR_OVERFLOW_TOTAL);
        }
    }
    int cap_err = ensure_capacity(parser, chunk_len);
    if (cap_err != SSE_OK) return sse_set_error(parser, cap_err);

    memcpy(parser->buf + parser->buf_len, chunk, chunk_len);
    parser->buf_len += chunk_len;
    parser->total_bytes_seen += chunk_len;

    size_t pos = 0;
    while (pos < parser->buf_len) {
        const char* line_start = parser->buf + pos;
        const char* nl = memchr(line_start, '\n', parser->buf_len - pos);
        if (!nl) {
            size_t partial_len = parser->buf_len - pos;
            if (parser->max_line_bytes && partial_len > parser->max_line_bytes) {
                return sse_set_error(parser, SSE_ERR_OVERFLOW_LINE);
            }
            break;
        }

        size_t line_len = (size_t)(nl - line_start);
        if (line_len > 0 && line_start[line_len - 1] == '\r') {
            line_len--;
        }
        if (parser->max_line_bytes && line_len > parser->max_line_bytes) {
            return sse_set_error(parser, SSE_ERR_OVERFLOW_LINE);
        }

        if (line_len == 0) {
            if (parser->on_frame) {
                if (!parser->on_frame(parser->frame_user_data)) {
                    return sse_set_error(parser, SSE_ERR_ABORT);
                }
            }
            parser->frame_bytes = 0;
            pos = (size_t)(nl - parser->buf) + 1;
            continue;
        }

        if (line_len >= 5 && memcmp(line_start, "data:", 5) == 0) {
            const char* payload = line_start + 5;
            size_t payload_len = line_len - 5;
            if (payload_len > 0 && payload[0] == ' ') {
                payload++;
                payload_len--;
            }
            if (parser->max_frame_bytes) {
                if (parser->frame_bytes > parser->max_frame_bytes) {
                    return sse_set_error(parser, SSE_ERR_OVERFLOW_FRAME);
                }
                size_t remaining = parser->max_frame_bytes - parser->frame_bytes;
                if (payload_len > remaining) {
                    return sse_set_error(parser, SSE_ERR_OVERFLOW_FRAME);
                }
            }
            parser->frame_bytes += payload_len;

            if (payload_len == 6 && memcmp(payload, "[DONE]", 6) == 0) {
                parser->is_done = true;
                pos = (size_t)(nl - parser->buf) + 1;
                break;
            }

            if (parser->on_data_line) {
                span_t line = {payload, payload_len};
                parser->on_data_line(parser->user_data, line);
            }
        }

        pos = (size_t)(nl - parser->buf) + 1;
    }

    if (pos > 0) {
        size_t remaining = parser->buf_len - pos;
        if (remaining > 0) {
            memmove(parser->buf, parser->buf + pos, remaining);
        }
        parser->buf_len = remaining;
    }

    return SSE_OK;
}
