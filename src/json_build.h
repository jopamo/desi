#ifndef JSON_BUILD_H
#define JSON_BUILD_H

#include <stddef.h>

#include "llm/llm.h"

char* build_chat_request(const char* model, const llm_message_t* messages, size_t messages_count, bool stream,
                         bool include_usage, const char* params_json, const char* tooling_json,
                         const char* response_format_json, size_t max_content_parts, size_t max_content_bytes);

char* build_completions_request(const char* model, const char* prompt, size_t prompt_len, bool stream,
                                bool include_usage, const char* params_json);

char* build_embeddings_request(const char* model, const llm_embedding_input_t* inputs, size_t inputs_count,
                               const char* params_json, size_t max_input_bytes, size_t max_inputs);

#endif  // JSON_BUILD_H
