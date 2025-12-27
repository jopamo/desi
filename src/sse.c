#include "sse.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct sse_parser sse_parser_t;

struct sse_buf {
    char* data;
    size_t len;
    size_t cap;
};

struct sse_parser {
    struct sse_buf line;
    struct sse_buf data;
    struct sse_buf event_type;
    struct sse_buf last_event_id;

    size_t max_line_bytes;
    size_t max_frame_bytes;
    size_t max_sse_buffer_bytes;
    size_t max_total_bytes;
    size_t total_bytes_seen;
    size_t mem_used;

    bool data_seen;
    bool pending_cr;

    bool bom_checked;
    unsigned char bom_buf[3];
    size_t bom_len;

    unsigned char utf8_seq[4];
    size_t utf8_len;
    size_t utf8_expected;
    size_t utf8_total;

    size_t retry_ms;
    bool retry_set;

    sse_event_cb on_event;
    void* event_user_data;
    sse_frame_cb on_frame;
    void* frame_user_data;

    int last_error;
};

static bool sse_size_add(size_t a, size_t b, size_t* out) {
    if (SIZE_MAX - a < b) return false;
    *out = a + b;
    return true;
}

static void sse_buf_free(struct sse_buf* buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static int sse_set_error(sse_parser_t* parser, int err) {
    if (parser && parser->last_error == SSE_OK) {
        parser->last_error = err;
    }
    return err;
}

static int sse_buf_reserve(sse_parser_t* parser, struct sse_buf* buf, size_t needed) {
    if (needed <= buf->cap) return SSE_OK;

    size_t new_cap = buf->cap ? buf->cap * 2 : 64;
    if (new_cap < needed) new_cap = needed;

    if (parser->max_sse_buffer_bytes) {
        size_t used_without = parser->mem_used - buf->cap;
        size_t max_cap = 0;
        if (parser->max_sse_buffer_bytes > used_without) {
            max_cap = parser->max_sse_buffer_bytes - used_without;
        }
        if (needed > max_cap) return SSE_ERR_OVERFLOW_BUFFER;
        if (new_cap > max_cap) new_cap = max_cap;
    }

    char* next = realloc(buf->data, new_cap);
    if (!next) return SSE_ERR_NOMEM;
    parser->mem_used = parser->mem_used - buf->cap + new_cap;
    buf->data = next;
    buf->cap = new_cap;
    return SSE_OK;
}

static int sse_buf_append(sse_parser_t* parser, struct sse_buf* buf, const char* data, size_t len) {
    if (len == 0) return SSE_OK;
    size_t needed = buf->len + len;
    int rc = sse_buf_reserve(parser, buf, needed);
    if (rc != SSE_OK) return rc;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return SSE_OK;
}

static int sse_buf_append_byte(sse_parser_t* parser, struct sse_buf* buf, unsigned char b) {
    char c = (char)b;
    return sse_buf_append(parser, buf, &c, 1);
}

static int sse_buf_set(sse_parser_t* parser, struct sse_buf* buf, const char* data, size_t len) {
    buf->len = 0;
    if (len == 0) return SSE_OK;
    int rc = sse_buf_reserve(parser, buf, len);
    if (rc != SSE_OK) return rc;
    memcpy(buf->data, data, len);
    buf->len = len;
    return SSE_OK;
}

static int sse_line_append(sse_parser_t* parser, const char* data, size_t len) {
    if (parser->max_line_bytes && parser->line.len + len > parser->max_line_bytes) {
        return SSE_ERR_OVERFLOW_LINE;
    }
    return sse_buf_append(parser, &parser->line, data, len);
}

static int sse_line_append_byte(sse_parser_t* parser, unsigned char b) {
    if (parser->max_line_bytes && parser->line.len + 1 > parser->max_line_bytes) {
        return SSE_ERR_OVERFLOW_LINE;
    }
    return sse_buf_append_byte(parser, &parser->line, b);
}

static int sse_append_replacement(sse_parser_t* parser) {
    static const unsigned char repl[] = {0xEF, 0xBF, 0xBD};
    return sse_line_append(parser, (const char*)repl, sizeof(repl));
}

static void utf8_reset(sse_parser_t* parser) {
    parser->utf8_len = 0;
    parser->utf8_expected = 0;
    parser->utf8_total = 0;
}

static bool utf8_sequence_valid(const unsigned char* seq, size_t len) {
    uint32_t code = 0;
    uint32_t min = 0;

    if (len == 2) {
        code = ((uint32_t)(seq[0] & 0x1F) << 6) | (uint32_t)(seq[1] & 0x3F);
        min = 0x80;
    } else if (len == 3) {
        code = ((uint32_t)(seq[0] & 0x0F) << 12) | ((uint32_t)(seq[1] & 0x3F) << 6) | (uint32_t)(seq[2] & 0x3F);
        min = 0x800;
    } else if (len == 4) {
        code = ((uint32_t)(seq[0] & 0x07) << 18) | ((uint32_t)(seq[1] & 0x3F) << 12) |
               ((uint32_t)(seq[2] & 0x3F) << 6) | (uint32_t)(seq[3] & 0x3F);
        min = 0x10000;
    } else {
        return false;
    }

    if (code < min) return false;
    if (code > 0x10FFFF) return false;
    if (code >= 0xD800 && code <= 0xDFFF) return false;
    return true;
}

static int sse_utf8_emit_sequence(sse_parser_t* parser) {
    if (utf8_sequence_valid(parser->utf8_seq, parser->utf8_len)) {
        return sse_line_append(parser, (const char*)parser->utf8_seq, parser->utf8_len);
    }
    return sse_append_replacement(parser);
}

static int sse_utf8_feed_byte(sse_parser_t* parser, unsigned char b);

static int sse_utf8_feed_byte(sse_parser_t* parser, unsigned char b) {
    if (parser->utf8_expected == 0) {
        if (b <= 0x7F) {
            return sse_line_append_byte(parser, b);
        }
        if (b >= 0xC2 && b <= 0xDF) {
            parser->utf8_seq[0] = b;
            parser->utf8_len = 1;
            parser->utf8_total = 2;
            parser->utf8_expected = 1;
            return SSE_OK;
        }
        if (b >= 0xE0 && b <= 0xEF) {
            parser->utf8_seq[0] = b;
            parser->utf8_len = 1;
            parser->utf8_total = 3;
            parser->utf8_expected = 2;
            return SSE_OK;
        }
        if (b >= 0xF0 && b <= 0xF4) {
            parser->utf8_seq[0] = b;
            parser->utf8_len = 1;
            parser->utf8_total = 4;
            parser->utf8_expected = 3;
            return SSE_OK;
        }
        return sse_append_replacement(parser);
    }

    if (b >= 0x80 && b <= 0xBF) {
        parser->utf8_seq[parser->utf8_len++] = b;
        parser->utf8_expected--;
        if (parser->utf8_expected == 0) {
            int rc = sse_utf8_emit_sequence(parser);
            utf8_reset(parser);
            return rc;
        }
        return SSE_OK;
    }

    int rc = sse_append_replacement(parser);
    utf8_reset(parser);
    if (rc != SSE_OK) return rc;
    return sse_utf8_feed_byte(parser, b);
}

static int sse_utf8_flush_incomplete(sse_parser_t* parser) {
    if (parser->utf8_expected == 0) return SSE_OK;
    utf8_reset(parser);
    return sse_append_replacement(parser);
}

static bool sse_parse_retry_value(const char* value, size_t len, size_t* out) {
    if (!value || len == 0 || !out) return false;
    size_t val = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < '0' || c > '9') return false;
        size_t digit = (size_t)(c - '0');
        if (val > (SIZE_MAX - digit) / 10) return false;
        val = (val * 10) + digit;
    }
    *out = val;
    return true;
}

static int sse_handle_data(sse_parser_t* parser, const char* value, size_t len) {
    size_t extra = len + 1;
    if (parser->max_frame_bytes && parser->data.len + extra > parser->max_frame_bytes) {
        return SSE_ERR_OVERFLOW_FRAME;
    }
    int rc = sse_buf_append(parser, &parser->data, value, len);
    if (rc != SSE_OK) return rc;
    rc = sse_buf_append_byte(parser, &parser->data, '\n');
    if (rc != SSE_OK) return rc;
    parser->data_seen = true;
    return SSE_OK;
}

static int sse_handle_event(sse_parser_t* parser, const char* value, size_t len) {
    return sse_buf_set(parser, &parser->event_type, value, len);
}

static int sse_handle_id(sse_parser_t* parser, const char* value, size_t len) {
    if (len > 0 && memchr(value, '\0', len)) return SSE_OK;
    return sse_buf_set(parser, &parser->last_event_id, value, len);
}

static int sse_process_line(sse_parser_t* parser) {
    if (parser->line.len == 0) {
        if (parser->on_frame) {
            if (!parser->on_frame(parser->frame_user_data)) {
                return SSE_ERR_ABORT;
            }
        }

        if (parser->data_seen) {
            if (parser->data.len > 0 && parser->data.data[parser->data.len - 1] == '\n') {
                parser->data.len--;
            }
            span_t event_type = {NULL, 0};
            if (parser->event_type.len > 0) {
                event_type.ptr = parser->event_type.data;
                event_type.len = parser->event_type.len;
            } else {
                event_type = span_from_cstr("message");
            }
            sse_event_t event = {
                .data = {.ptr = parser->data.len ? parser->data.data : NULL, .len = parser->data.len},
                .event_type = event_type,
                .last_event_id = {.ptr = parser->last_event_id.len ? parser->last_event_id.data : NULL,
                                  .len = parser->last_event_id.len},
            };
            if (parser->on_event) {
                if (!parser->on_event(parser->event_user_data, &event)) {
                    return SSE_ERR_ABORT;
                }
            }
        }

        parser->data.len = 0;
        parser->event_type.len = 0;
        parser->data_seen = false;
        return SSE_OK;
    }

    if (parser->line.data[0] == ':') return SSE_OK;

    const char* line = parser->line.data;
    size_t len = parser->line.len;
    const char* colon = memchr(line, ':', len);
    const char* field = line;
    size_t field_len = 0;
    const char* value = NULL;
    size_t value_len = 0;

    if (colon) {
        field_len = (size_t)(colon - line);
        value = colon + 1;
        value_len = len - field_len - 1;
        if (value_len > 0 && value[0] == ' ') {
            value++;
            value_len--;
        }
    } else {
        field_len = len;
        value = "";
        value_len = 0;
    }

    if (field_len == 4 && memcmp(field, "data", 4) == 0) {
        return sse_handle_data(parser, value, value_len);
    }
    if (field_len == 5 && memcmp(field, "event", 5) == 0) {
        return sse_handle_event(parser, value, value_len);
    }
    if (field_len == 2 && memcmp(field, "id", 2) == 0) {
        return sse_handle_id(parser, value, value_len);
    }
    if (field_len == 5 && memcmp(field, "retry", 5) == 0) {
        size_t retry = 0;
        if (sse_parse_retry_value(value, value_len, &retry)) {
            parser->retry_ms = retry;
            parser->retry_set = true;
        }
        return SSE_OK;
    }

    return SSE_OK;
}

static int sse_process_raw_byte(sse_parser_t* parser, unsigned char b) {
    if (parser->pending_cr) {
        if (b == '\n') {
            parser->pending_cr = false;
            return SSE_OK;
        }
        parser->pending_cr = false;
    }

    if (b == '\r') {
        int rc = sse_utf8_flush_incomplete(parser);
        if (rc != SSE_OK) return rc;
        rc = sse_process_line(parser);
        if (rc != SSE_OK) return rc;
        parser->line.len = 0;
        parser->pending_cr = true;
        return SSE_OK;
    }

    if (b == '\n') {
        int rc = sse_utf8_flush_incomplete(parser);
        if (rc != SSE_OK) return rc;
        rc = sse_process_line(parser);
        if (rc != SSE_OK) return rc;
        parser->line.len = 0;
        return SSE_OK;
    }

    return sse_utf8_feed_byte(parser, b);
}

static int sse_process_byte(sse_parser_t* parser, unsigned char b) {
    if (!parser->bom_checked) {
        parser->bom_buf[parser->bom_len++] = b;
        if (parser->bom_len == 1) {
            if (parser->bom_buf[0] == 0xEF) return SSE_OK;
        } else if (parser->bom_len == 2) {
            if (parser->bom_buf[1] == 0xBB) return SSE_OK;
        } else if (parser->bom_len == 3) {
            if (parser->bom_buf[2] == 0xBF) {
                parser->bom_checked = true;
                parser->bom_len = 0;
                return SSE_OK;
            }
        } else {
            parser->bom_len = 3;
        }

        parser->bom_checked = true;
        for (size_t i = 0; i < parser->bom_len; i++) {
            int rc = sse_process_raw_byte(parser, parser->bom_buf[i]);
            if (rc != SSE_OK) return rc;
        }
        parser->bom_len = 0;
        return SSE_OK;
    }

    return sse_process_raw_byte(parser, b);
}

sse_parser_t* sse_create(size_t max_line_bytes, size_t max_frame_bytes, size_t max_sse_buffer_bytes,
                         size_t max_total_bytes) {
    sse_parser_t* parser = malloc(sizeof(*parser));
    if (!parser) return NULL;
    memset(parser, 0, sizeof(*parser));
    parser->max_line_bytes = max_line_bytes;
    parser->max_frame_bytes = max_frame_bytes;
    parser->max_sse_buffer_bytes = max_sse_buffer_bytes;
    parser->max_total_bytes = max_total_bytes;
    parser->last_error = SSE_OK;
    return parser;
}

void sse_destroy(sse_parser_t* parser) {
    if (!parser) return;
    sse_buf_free(&parser->line);
    sse_buf_free(&parser->data);
    sse_buf_free(&parser->event_type);
    sse_buf_free(&parser->last_event_id);
    free(parser);
}

void sse_set_callback(sse_parser_t* parser, sse_event_cb cb, void* user_data) {
    if (!parser) return;
    parser->on_event = cb;
    parser->event_user_data = user_data;
}

void sse_set_frame_callback(sse_parser_t* parser, sse_frame_cb cb, void* user_data) {
    if (!parser) return;
    parser->on_frame = cb;
    parser->frame_user_data = user_data;
}

int sse_feed(sse_parser_t* parser, const char* chunk, size_t chunk_len) {
    if (!parser) return SSE_ERR_NOMEM;
    if (parser->last_error != SSE_OK) return parser->last_error;
    if (!chunk || chunk_len == 0) return SSE_OK;

    if (parser->max_total_bytes) {
        size_t remaining = parser->max_total_bytes - parser->total_bytes_seen;
        if (chunk_len > remaining) {
            return sse_set_error(parser, SSE_ERR_OVERFLOW_TOTAL);
        }
    }
    parser->total_bytes_seen += chunk_len;

    for (size_t i = 0; i < chunk_len; i++) {
        int rc = sse_process_byte(parser, (unsigned char)chunk[i]);
        if (rc != SSE_OK) {
            return sse_set_error(parser, rc);
        }
    }

    return SSE_OK;
}

bool sse_retry_ms(const sse_parser_t* parser, size_t* out) {
    if (!parser || !out || !parser->retry_set) return false;
    *out = parser->retry_ms;
    return true;
}

int sse_write_event(const sse_write_limits_t* limits, const char* event_type, size_t event_len, const char* data,
                    size_t data_len, char* out, size_t out_cap, size_t* out_len) {
    if (!out || !out_len) return SSE_ERR_BAD_INPUT;
    *out_len = 0;
    if (event_len > 0 && !event_type) return SSE_ERR_BAD_INPUT;
    if (data_len > 0 && !data) return SSE_ERR_BAD_INPUT;

    size_t max_line = limits ? limits->max_line_bytes : 0;
    size_t max_frame = limits ? limits->max_frame_bytes : 0;
    const size_t event_prefix = sizeof("event: ") - 1;
    const size_t data_prefix = sizeof("data: ") - 1;
    // Include the optional space so leading spaces survive field parsing.
    const size_t event_line_overhead = event_prefix + 1;
    const size_t data_line_overhead = data_prefix + 1;

    if (max_frame) {
        if (data_len == SIZE_MAX || data_len + 1 > max_frame) return SSE_ERR_OVERFLOW_FRAME;
    }

    if (event_len > 0) {
        if (memchr(event_type, '\n', event_len) || memchr(event_type, '\r', event_len)) return SSE_ERR_BAD_INPUT;
        if (max_line && (max_line < event_prefix || event_len > max_line - event_prefix)) return SSE_ERR_OVERFLOW_LINE;
    }

    size_t lines = 0;
    size_t sum_line_len = 0;
    size_t line_start = 0;

    for (size_t i = 0; i <= data_len; i++) {
        if (i < data_len && data[i] == '\r') return SSE_ERR_BAD_INPUT;
        if (i == data_len || data[i] == '\n') {
            size_t line_len = i - line_start;
            if (max_line && (max_line < data_prefix || line_len > max_line - data_prefix)) {
                return SSE_ERR_OVERFLOW_LINE;
            }
            if (!sse_size_add(sum_line_len, line_len, &sum_line_len)) return SSE_ERR_OVERFLOW_BUFFER;
            if (lines == SIZE_MAX) return SSE_ERR_OVERFLOW_BUFFER;
            lines++;
            line_start = i + 1;
        }
    }

    size_t total = 0;
    if (event_len > 0) {
        size_t event_total = 0;
        if (!sse_size_add(event_line_overhead, event_len, &event_total)) return SSE_ERR_OVERFLOW_BUFFER;
        if (!sse_size_add(total, event_total, &total)) return SSE_ERR_OVERFLOW_BUFFER;
    }
    if (lines != 0 && data_line_overhead > 0 && lines > SIZE_MAX / data_line_overhead) return SSE_ERR_OVERFLOW_BUFFER;
    if (!sse_size_add(total, lines * data_line_overhead, &total)) return SSE_ERR_OVERFLOW_BUFFER;
    if (!sse_size_add(total, sum_line_len, &total)) return SSE_ERR_OVERFLOW_BUFFER;
    if (!sse_size_add(total, 1, &total)) return SSE_ERR_OVERFLOW_BUFFER;

    if (total > out_cap) return SSE_ERR_OVERFLOW_BUFFER;

    char* p = out;
    if (event_len > 0) {
        memcpy(p, "event: ", event_prefix);
        p += event_prefix;
        if (event_len > 0) {
            memcpy(p, event_type, event_len);
            p += event_len;
        }
        *p++ = '\n';
    }

    line_start = 0;
    for (size_t i = 0; i <= data_len; i++) {
        if (i == data_len || data[i] == '\n') {
            size_t line_len = i - line_start;
            memcpy(p, "data: ", data_prefix);
            p += data_prefix;
            if (line_len > 0) {
                memcpy(p, data + line_start, line_len);
                p += line_len;
            }
            *p++ = '\n';
            line_start = i + 1;
        }
    }
    *p++ = '\n';

    *out_len = total;
    return SSE_OK;
}

int sse_write_keepalive(const sse_write_limits_t* limits, char* out, size_t out_cap, size_t* out_len) {
    if (!out || !out_len) return SSE_ERR_BAD_INPUT;
    *out_len = 0;

    size_t max_line = limits ? limits->max_line_bytes : 0;
    const char* keepalive = ": ping";
    size_t line_len = strlen(keepalive);
    size_t total = line_len + 2;

    if (max_line && line_len > max_line) return SSE_ERR_OVERFLOW_LINE;
    if (total > out_cap) return SSE_ERR_OVERFLOW_BUFFER;

    memcpy(out, keepalive, line_len);
    out[line_len] = '\n';
    out[line_len + 1] = '\n';
    *out_len = total;
    return SSE_OK;
}
