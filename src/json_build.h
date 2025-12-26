#ifndef JSON_BUILD_H
#define JSON_BUILD_H

#include <stddef.h>

#include "llm/llm.h"

char* build_chat_request(const char* model, const llm_message_t* messages, size_t messages_count, bool stream,
                         const char* params_json, const char* tooling_json, const char* response_format_json);

char* build_completions_request(const char* model, const char* prompt, size_t prompt_len, bool stream,
                                const char* params_json);

#endif  // JSON_BUILD_H
