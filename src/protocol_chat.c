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

static void free_chat_choices(llm_chat_choice_t* choices, size_t count) {
    if (!choices) return;
    for (size_t i = 0; i < count; i++) {
        free(choices[i].tool_calls);
    }
    free(choices);
}

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

    size_t choices_count = (size_t)tokens[choices_idx].size;
    result->choices = calloc(choices_count, sizeof(llm_chat_choice_t));
    if (!result->choices) {
        free_tokens(tokens);
        return JSTOK_ERROR_NOMEM;
    }
    result->choices_count = choices_count;

    for (size_t i = 0; i < choices_count; i++) {
        int choice_idx = arr_get(tokens, count, choices_idx, (int)i);
        if (choice_idx < 0 || tokens[choice_idx].type != JSTOK_OBJECT) {
            free_chat_choices(result->choices, result->choices_count);
            result->choices = NULL;
            result->choices_count = 0;
            free_tokens(tokens);
            return -1;
        }

        llm_chat_choice_t* choice = &result->choices[i];
        choice->finish_reason = LLM_FINISH_REASON_UNKNOWN;

        int finish_idx = obj_get_key(tokens, count, choice_idx, json, "finish_reason");
        if (finish_idx >= 0 && tokens[finish_idx].type == JSTOK_STRING) {
            span_t sp = tok_span(json, &tokens[finish_idx]);
            choice->finish_reason = llm_finish_reason_from_string(sp.ptr, sp.len);
        }

        int message_idx = obj_get_key(tokens, count, choice_idx, json, "message");
        if (message_idx < 0 || tokens[message_idx].type != JSTOK_OBJECT) {
            free_chat_choices(result->choices, result->choices_count);
            result->choices = NULL;
            result->choices_count = 0;
            free_tokens(tokens);
            return -1;
        }

        int content_idx = obj_get_key(tokens, count, message_idx, json, "content");
        if (content_idx >= 0 && tokens[content_idx].type == JSTOK_STRING) {
            span_t sp = tok_span(json, &tokens[content_idx]);
            choice->content = sp.ptr;
            choice->content_len = sp.len;
        }
        int reasoning_idx = obj_get_key(tokens, count, message_idx, json, "reasoning_content");
        if (reasoning_idx >= 0 && tokens[reasoning_idx].type == JSTOK_STRING) {
            span_t sp = tok_span(json, &tokens[reasoning_idx]);
            choice->reasoning_content = sp.ptr;
            choice->reasoning_content_len = sp.len;
        }
        int tool_calls_idx = obj_get_key(tokens, count, message_idx, json, "tool_calls");
        if (tool_calls_idx >= 0 && tokens[tool_calls_idx].type == JSTOK_ARRAY) {
            span_t sp = tok_span(json, &tokens[tool_calls_idx]);
            choice->tool_calls_json = sp.ptr;
            choice->tool_calls_json_len = sp.len;
            if (tokens[tool_calls_idx].size > 0) {
                size_t tool_count = (size_t)tokens[tool_calls_idx].size;
                choice->tool_calls = calloc(tool_count, sizeof(llm_tool_call_t));
                if (!choice->tool_calls) {
                    free_chat_choices(result->choices, result->choices_count);
                    result->choices = NULL;
                    result->choices_count = 0;
                    free_tokens(tokens);
                    return JSTOK_ERROR_NOMEM;
                }
                choice->tool_calls_count = tool_count;
                for (size_t j = 0; j < tool_count; j++) {
                    int tool_idx = arr_get(tokens, count, tool_calls_idx, (int)j);
                    if (tool_idx < 0 || tokens[tool_idx].type != JSTOK_OBJECT) {
                        free_chat_choices(result->choices, result->choices_count);
                        result->choices = NULL;
                        result->choices_count = 0;
                        free_tokens(tokens);
                        return -1;
                    }
                    llm_tool_call_t* tc = &choice->tool_calls[j];
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
    }

    if (result->choices_count > 0) {
        llm_chat_choice_t* choice0 = &result->choices[0];
        result->finish_reason = choice0->finish_reason;
        result->content = choice0->content;
        result->content_len = choice0->content_len;
        result->reasoning_content = choice0->reasoning_content;
        result->reasoning_content_len = choice0->reasoning_content_len;
        result->tool_calls = choice0->tool_calls;
        result->tool_calls_count = choice0->tool_calls_count;
        result->tool_calls_json = choice0->tool_calls_json;
        result->tool_calls_json_len = choice0->tool_calls_json_len;
    }

    free_tokens(tokens);
    return 0;
}

static bool parse_choice_index(const char* json, const jstoktok_t* tok, size_t* out) {
    if (!tok || tok->type != JSTOK_PRIMITIVE) return false;
    span_t sp = tok_span(json, tok);
    if (sp.len == 0) return false;
    size_t val = 0;
    for (size_t i = 0; i < sp.len; i++) {
        char c = sp.ptr[i];
        if (c < '0' || c > '9') return false;
        val = (val * 10) + (size_t)(c - '0');
    }
    *out = val;
    return true;
}

static int find_choice_token(const char* json, const jstoktok_t* tokens, int count, int choices_idx,
                             size_t choice_index) {
    int size = tokens[choices_idx].size;
    if (size <= 0) return -1;
    int fallback = -1;
    for (int i = 0; i < size; i++) {
        int choice_idx = arr_get(tokens, count, choices_idx, i);
        if (choice_idx < 0 || tokens[choice_idx].type != JSTOK_OBJECT) continue;
        if (choice_index == 0 && fallback < 0) fallback = choice_idx;
        int index_idx = obj_get_key(tokens, count, choice_idx, json, "index");
        size_t idx_val = 0;
        if (index_idx >= 0 && parse_choice_index(json, &tokens[index_idx], &idx_val) && idx_val == choice_index) {
            return choice_idx;
        }
    }
    if (choice_index == 0) return fallback;
    return -1;
}

int parse_chat_chunk_choice(const char* json, size_t len, size_t choice_index, llm_chat_chunk_delta_t* delta) {
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
        int choice_idx = find_choice_token(json, tokens, count, choices_idx, choice_index);
        if (choice_idx < 0) {
            free_tokens(tokens);
            return 0;
        }
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
            if (tool_calls_idx >= 0 && tokens[tool_calls_idx].type == JSTOK_ARRAY && tokens[tool_calls_idx].size > 0) {
                int tool_count = tokens[tool_calls_idx].size;
                delta->tool_call_deltas = calloc((size_t)tool_count, sizeof(llm_tool_call_delta_t));
                if (!delta->tool_call_deltas) {
                    free_tokens(tokens);
                    return JSTOK_ERROR_NOMEM;
                }
                delta->tool_call_deltas_count = (size_t)tool_count;
                for (int i = 0; i < tool_count; i++) {
                    int tool_idx = arr_get(tokens, count, tool_calls_idx, i);
                    if (tool_idx < 0 || tokens[tool_idx].type != JSTOK_OBJECT) {
                        free(delta->tool_call_deltas);
                        delta->tool_call_deltas = NULL;
                        delta->tool_call_deltas_count = 0;
                        free_tokens(tokens);
                        return -1;
                    }
                    llm_tool_call_delta_t* td = &delta->tool_call_deltas[i];
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

    free_tokens(tokens);
    return 0;
}

int parse_chat_chunk(const char* json, size_t len, llm_chat_chunk_delta_t* delta) {
    return parse_chat_chunk_choice(json, len, 0, delta);
}
