#ifndef SSE_H
#define SSE_H

#include <stdbool.h>
#include <stddef.h>

#include "llm/internal.h"

typedef struct sse_parser sse_parser_t;

typedef enum {
    SSE_OK = 0,
    SSE_ERR_NOMEM = -1,
    SSE_ERR_OVERFLOW_LINE = -2,
    SSE_ERR_OVERFLOW_FRAME = -3,
    SSE_ERR_OVERFLOW_BUFFER = -4,
    SSE_ERR_OVERFLOW_TOTAL = -5,
    SSE_ERR_ABORT = -6,
    SSE_ERR_BAD_INPUT = -7,
} sse_err_t;

typedef bool (*sse_frame_cb)(void* user_data);

typedef struct {
    span_t data;
    span_t event_type;
    span_t last_event_id;
} sse_event_t;

typedef bool (*sse_event_cb)(void* user_data, const sse_event_t* event);

typedef struct {
    size_t max_line_bytes;
    size_t max_frame_bytes;
} sse_write_limits_t;

sse_parser_t* sse_create(size_t max_line_bytes, size_t max_frame_bytes, size_t max_sse_buffer_bytes,
                         size_t max_total_bytes);
void sse_destroy(sse_parser_t* parser);
void sse_set_callback(sse_parser_t* parser, sse_event_cb cb, void* user_data);
void sse_set_frame_callback(sse_parser_t* parser, sse_frame_cb cb, void* user_data);
int sse_feed(sse_parser_t* parser, const char* chunk, size_t chunk_len);
bool sse_retry_ms(const sse_parser_t* parser, size_t* out);
int sse_write_event(const sse_write_limits_t* limits, const char* event_type, size_t event_len, const char* data,
                    size_t data_len, char* out, size_t out_cap, size_t* out_len);
int sse_write_keepalive(const sse_write_limits_t* limits, char* out, size_t out_cap, size_t* out_len);

#endif  // SSE_H
