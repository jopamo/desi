#ifndef SSE_H
#define SSE_H

#include <stdbool.h>
#include <stddef.h>

#include "llm/internal.h"

typedef struct sse_parser sse_parser_t;

sse_parser_t* sse_create(size_t max_line_bytes, size_t max_total_bytes);
void sse_destroy(sse_parser_t* parser);
void sse_set_callback(sse_parser_t* parser, void (*cb)(void* user_data, span_t line), void* user_data);
int sse_feed(sse_parser_t* parser, const char* chunk, size_t chunk_len);
bool sse_is_done(sse_parser_t* parser);

#endif  // SSE_H
