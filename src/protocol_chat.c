#include "llm/internal.h"
#include "llm/json_core.h"
#include "llm/llm.h"
#define JSTOK_HEADER
#include <jstok.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tokenize(const char* json, size_t len, jstoktok_t** tokens_out, int* count_out) {
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed < 0) return needed;

    jstoktok_t* tokens = malloc(needed * sizeof(jstoktok_t));
    if (!tokens) return JSTOK_ERROR_NOMEM;

    jstok_init(&parser);
    int parsed = jstok_parse(&parser, json, (int)len, tokens, needed);
    if (parsed < 0) {
        free(tokens);
        return parsed;
    }
    *tokens_out = tokens;
    *count_out = parsed;
    return 0;
}

static void free_tokens(jstoktok_t* tokens) { free(tokens); }

int parse_chat_response(const char* json, size_t len, llm_chat_result_t* result) {
    jstoktok_t* tokens = NULL;
    int count = 0;
    int ret = tokenize(json, len, &tokens, &count);
    if (ret < 0) return ret;

    if (count == 0 || tokens[0].type != JSTOK_OBJECT) {
        free_tokens(tokens);
        return -1;
    }

    memset(result, 0, sizeof(*result));

    int choices_idx = obj_get_key(tokens, count, 0, json, "choices");
    if (choices_idx < 0 || tokens[choices_idx].type != JSTOK_ARRAY || tokens[choices_idx].size <= 0) {
        free_tokens(tokens);
        return -1;
    }

    int choice_idx = arr_get(tokens, count, choices_idx, 0);
    if (choice_idx < 0 || tokens[choice_idx].type != JSTOK_OBJECT) {
        free_tokens(tokens);
        return -1;
    }

    int finish_idx = obj_get_key(tokens, count, choice_idx, json, "finish_reason");
    if (finish_idx >= 0 && tokens[finish_idx].type == JSTOK_STRING) {
        span_t sp = tok_span(json, &tokens[finish_idx]);
        result->finish_reason = llm_finish_reason_from_string(sp.ptr, sp.len);
    }

    int message_idx = obj_get_key(tokens, count, choice_idx, json, "message");
    if (message_idx < 0 || tokens[message_idx].type != JSTOK_OBJECT) {
        free_tokens(tokens);
        return -1;
    }

    int content_idx = obj_get_key(tokens, count, message_idx, json, "content");
    if (content_idx >= 0 && tokens[content_idx].type == JSTOK_STRING) {
        span_t sp = tok_span(json, &tokens[content_idx]);
        result->content = sp.ptr;
        result->content_len = sp.len;
    }
    int reasoning_idx = obj_get_key(tokens, count, message_idx, json, "reasoning_content");
    if (reasoning_idx >= 0 && tokens[reasoning_idx].type == JSTOK_STRING) {
        span_t sp = tok_span(json, &tokens[reasoning_idx]);
        result->reasoning_content = sp.ptr;
        result->reasoning_content_len = sp.len;
    }
    int tool_calls_idx = obj_get_key(tokens, count, message_idx, json, "tool_calls");
    if (tool_calls_idx >= 0 && tokens[tool_calls_idx].type == JSTOK_ARRAY && tokens[tool_calls_idx].size > 0) {
        int tool_count = tokens[tool_calls_idx].size;
        result->tool_calls_count = tool_count;
        result->tool_calls = malloc(tool_count * sizeof(llm_tool_call_t));
        if (result->tool_calls) {
            for (int i = 0; i < tool_count; i++) {
                int tool_idx = arr_get(tokens, count, tool_calls_idx, i);
                if (tool_idx < 0 || tokens[tool_idx].type != JSTOK_OBJECT) continue;
                llm_tool_call_t* tc = &result->tool_calls[i];
                memset(tc, 0, sizeof(*tc));
                int id_idx = obj_get_key(tokens, count, tool_idx, json, "id");
                if (id_idx >= 0 && tokens[id_idx].type == JSTOK_STRING) {
                    span_t sp = tok_span(json, &tokens[id_idx]);
                    tc->id = sp.ptr;
                    tc->id_len = sp.len;
                }
                int func_idx = obj_get_key(tokens, count, tool_idx, json, "function");
                if (func_idx >= 0 && tokens[func_idx].type == JSTOK_OBJECT) {
                    int name_idx = obj_get_key(tokens, count, func_idx, json, "name");
                    if (name_idx >= 0 && tokens[name_idx].type == JSTOK_STRING) {
                        span_t sp = tok_span(json, &tokens[name_idx]);
                        tc->name = sp.ptr;
                        tc->name_len = sp.len;
                    }
                    int args_idx = obj_get_key(tokens, count, func_idx, json, "arguments");
                    if (args_idx >= 0 && tokens[args_idx].type == JSTOK_STRING) {
                        span_t sp = tok_span(json, &tokens[args_idx]);
                        tc->arguments = sp.ptr;
                        tc->arguments_len = sp.len;
                    }
                }
            }
        }
    }

    free_tokens(tokens);
    return 0;
}

int parse_chat_chunk(const char* json, size_t len, llm_chat_chunk_delta_t* delta) {
    jstoktok_t* tokens = NULL;
    int count = 0;
    int ret = tokenize(json, len, &tokens, &count);
    if (ret < 0) return ret;

    if (count == 0 || tokens[0].type != JSTOK_OBJECT) {
        free_tokens(tokens);
        return -1;
    }

    memset(delta, 0, sizeof(*delta));
    delta->finish_reason = LLM_FINISH_REASON_UNKNOWN;

    int choices_idx = obj_get_key(tokens, count, 0, json, "choices");
    if (choices_idx >= 0 && tokens[choices_idx].type == JSTOK_ARRAY && tokens[choices_idx].size > 0) {
        int choice_idx = arr_get(tokens, count, choices_idx, 0);
        if (choice_idx >= 0 && tokens[choice_idx].type == JSTOK_OBJECT) {
            int finish_idx = obj_get_key(tokens, count, choice_idx, json, "finish_reason");
            if (finish_idx >= 0 && tokens[finish_idx].type == JSTOK_STRING) {
                span_t sp = tok_span(json, &tokens[finish_idx]);
                delta->finish_reason = llm_finish_reason_from_string(sp.ptr, sp.len);
            }
            int delta_obj_idx = obj_get_key(tokens, count, choice_idx, json, "delta");
            if (delta_obj_idx >= 0 && tokens[delta_obj_idx].type == JSTOK_OBJECT) {
                int content_idx = obj_get_key(tokens, count, delta_obj_idx, json, "content");
                if (content_idx >= 0 && tokens[content_idx].type == JSTOK_STRING) {
                    span_t sp = tok_span(json, &tokens[content_idx]);
                    delta->content_delta = sp.ptr;
                    delta->content_delta_len = sp.len;
                }
                int reasoning_idx = obj_get_key(tokens, count, delta_obj_idx, json, "reasoning_content");
                if (reasoning_idx >= 0 && tokens[reasoning_idx].type == JSTOK_STRING) {
                    span_t sp = tok_span(json, &tokens[reasoning_idx]);
                    delta->reasoning_delta = sp.ptr;
                    delta->reasoning_delta_len = sp.len;
                }
                int tool_calls_idx = obj_get_key(tokens, count, delta_obj_idx, json, "tool_calls");
                if (tool_calls_idx >= 0 && tokens[tool_calls_idx].type == JSTOK_ARRAY &&
                    tokens[tool_calls_idx].size > 0) {
                    int tool_count = tokens[tool_calls_idx].size;
                    delta->tool_call_deltas_count = tool_count;
                    delta->tool_call_deltas = malloc(tool_count * sizeof(llm_tool_call_delta_t));
                    if (delta->tool_call_deltas) {
                        for (int i = 0; i < tool_count; i++) {
                            int tool_idx = arr_get(tokens, count, tool_calls_idx, i);
                            if (tool_idx < 0 || tokens[tool_idx].type != JSTOK_OBJECT) continue;
                            llm_tool_call_delta_t* td = &delta->tool_call_deltas[i];
                            memset(td, 0, sizeof(*td));
                            int index_idx = obj_get_key(tokens, count, tool_idx, json, "index");
                            if (index_idx >= 0 && tokens[index_idx].type == JSTOK_PRIMITIVE) {
                                span_t sp = tok_span(json, &tokens[index_idx]);
                                td->index = (size_t)atoi(sp.ptr);
                            }
                            int id_idx = obj_get_key(tokens, count, tool_idx, json, "id");
                            if (id_idx >= 0 && tokens[id_idx].type == JSTOK_STRING) {
                                span_t sp = tok_span(json, &tokens[id_idx]);
                                td->id = sp.ptr;
                                td->id_len = sp.len;
                            }
                            int func_idx = obj_get_key(tokens, count, tool_idx, json, "function");
                            if (func_idx >= 0 && tokens[func_idx].type == JSTOK_OBJECT) {
                                int name_idx = obj_get_key(tokens, count, func_idx, json, "name");
                                if (name_idx >= 0 && tokens[name_idx].type == JSTOK_STRING) {
                                    span_t sp = tok_span(json, &tokens[name_idx]);
                                    td->name = sp.ptr;
                                    td->name_len = sp.len;
                                }
                                int args_idx = obj_get_key(tokens, count, func_idx, json, "arguments");
                                if (args_idx >= 0 && tokens[args_idx].type == JSTOK_STRING) {
                                    span_t sp = tok_span(json, &tokens[args_idx]);
                                    td->arguments_fragment = sp.ptr;
                                    td->arguments_fragment_len = sp.len;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    free_tokens(tokens);
    return 0;
}
