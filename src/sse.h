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
} sse_err_t;

typedef bool (*sse_frame_cb)(void* user_data);

sse_parser_t* sse_create(size_t max_line_bytes, size_t max_frame_bytes, size_t max_sse_buffer_bytes,
                         size_t max_total_bytes);
void sse_destroy(sse_parser_t* parser);
void sse_set_callback(sse_parser_t* parser, void (*cb)(void* user_data, span_t line), void* user_data);
void sse_set_frame_callback(sse_parser_t* parser, sse_frame_cb cb, void* user_data);
int sse_feed(sse_parser_t* parser, const char* chunk, size_t chunk_len);
bool sse_is_done(sse_parser_t* parser);

#endif  // SSE_H
