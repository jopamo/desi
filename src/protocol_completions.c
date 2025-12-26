#include "llm/internal.h"
#include "llm/json_core.h"
#include "llm/llm.h"
#define JSTOK_HEADER
#include <jstok.h>
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

int parse_completions_response(const char* json, size_t len, llm_completions_result_t* result) {
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
    result->choices = calloc(choices_count, sizeof(llm_completion_choice_t));
    if (!result->choices) {
        free_tokens(tokens);
        return JSTOK_ERROR_NOMEM;
    }
    result->choices_count = choices_count;

    for (size_t i = 0; i < choices_count; i++) {
        int choice_idx = arr_get(tokens, count, choices_idx, (int)i);
        if (choice_idx < 0 || tokens[choice_idx].type != JSTOK_OBJECT) {
            free(result->choices);
            result->choices = NULL;
            result->choices_count = 0;
            free_tokens(tokens);
            return -1;
        }
        int text_idx = obj_get_key(tokens, count, choice_idx, json, "text");
        if (text_idx < 0 || tokens[text_idx].type != JSTOK_STRING) {
            free(result->choices);
            result->choices = NULL;
            result->choices_count = 0;
            free_tokens(tokens);
            return -1;
        }
        span_t sp = tok_span(json, &tokens[text_idx]);
        result->choices[i].text = sp.ptr;
        result->choices[i].text_len = sp.len;
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

int parse_completions_chunk_choice(const char* json, size_t len, size_t choice_index, span_t* text_delta,
                                   llm_finish_reason_t* finish_reason) {
    jstoktok_t* tokens = NULL;
    int count = 0;
    int ret = tokenize(json, len, &tokens, &count);
    if (ret < 0) return ret;

    if (text_delta) {
        text_delta->ptr = NULL;
        text_delta->len = 0;
    }
    if (finish_reason) {
        *finish_reason = LLM_FINISH_REASON_UNKNOWN;
    }

    if (count == 0 || tokens[0].type != JSTOK_OBJECT) {
        free_tokens(tokens);
        return -1;
    }

    int choices_idx = obj_get_key(tokens, count, 0, json, "choices");
    if (choices_idx >= 0 && tokens[choices_idx].type == JSTOK_ARRAY && tokens[choices_idx].size > 0) {
        int choice_idx = find_choice_token(json, tokens, count, choices_idx, choice_index);
        if (choice_idx >= 0 && tokens[choice_idx].type == JSTOK_OBJECT) {
            int text_idx = obj_get_key(tokens, count, choice_idx, json, "text");
            if (text_idx >= 0 && tokens[text_idx].type == JSTOK_STRING) {
                span_t sp = tok_span(json, &tokens[text_idx]);
                if (text_delta) {
                    text_delta->ptr = sp.ptr;
                    text_delta->len = sp.len;
                }
            }

            int finish_idx = obj_get_key(tokens, count, choice_idx, json, "finish_reason");
            if (finish_idx >= 0 && tokens[finish_idx].type == JSTOK_STRING) {
                span_t sp = tok_span(json, &tokens[finish_idx]);
                if (finish_reason) {
                    *finish_reason = llm_finish_reason_from_string(sp.ptr, sp.len);
                }
            }
        }
    }

    free_tokens(tokens);
    return 0;
}

int parse_completions_chunk(const char* json, size_t len, span_t* text_delta, llm_finish_reason_t* finish_reason) {
    return parse_completions_chunk_choice(json, len, 0, text_delta, finish_reason);
}
