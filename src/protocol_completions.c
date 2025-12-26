#include "llm/internal.h"
#include "llm/json_core.h"
#include "llm/llm.h"
#define JSTOK_HEADER
#include <jstok.h>
#include <stdlib.h>
#include <string.h>

int parse_completions_response(const char* json, size_t len, const char*** texts, size_t* count) {
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return -1;

    jstoktok_t* tokens = malloc(needed * sizeof(jstoktok_t));
    if (!tokens) return -1;

    jstok_init(&parser);
    jstok_parse(&parser, json, (int)len, tokens, needed);

    *texts = NULL;
    *count = 0;

    int choices_idx = jstok_object_get(json, tokens, needed, 0, "choices");
    if (choices_idx < 0 || tokens[choices_idx].type != JSTOK_ARRAY || tokens[choices_idx].size <= 0) {
        free(tokens);
        return -1;
    }

    int n = tokens[choices_idx].size;
    *texts = malloc(n * sizeof(char*));
    for (int i = 0; i < n; i++) {
        int choice_idx = jstok_array_at(tokens, needed, choices_idx, i);
        if (choice_idx >= 0 && tokens[choice_idx].type == JSTOK_OBJECT) {
            int text_idx = jstok_object_get(json, tokens, needed, choice_idx, "text");
            if (text_idx >= 0 && tokens[text_idx].type == JSTOK_STRING) {
                jstok_span_t sp = jstok_span(json, &tokens[text_idx]);
                char* t = malloc(sp.n + 1);
                memcpy(t, sp.p, sp.n);
                t[sp.n] = '\0';
                (*texts)[*count] = t;
                (*count)++;
            }
        }
    }

    free(tokens);
    return 0;
}

int parse_completions_chunk(const char* json, size_t len, span_t* text_delta, llm_finish_reason_t* finish_reason) {
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return -1;

    jstoktok_t* tokens = malloc(needed * sizeof(jstoktok_t));
    if (!tokens) return JSTOK_ERROR_NOMEM;

    jstok_init(&parser);
    jstok_parse(&parser, json, (int)len, tokens, needed);

    if (text_delta) {
        text_delta->ptr = NULL;
        text_delta->len = 0;
    }
    if (finish_reason) {
        *finish_reason = LLM_FINISH_REASON_UNKNOWN;
    }

    // Check for OpenAI error format? Assuming happy path or simple structure
    int choices_idx = jstok_object_get(json, tokens, needed, 0, "choices");
    if (choices_idx >= 0 && tokens[choices_idx].type == JSTOK_ARRAY && tokens[choices_idx].size > 0) {
        int choice_idx = jstok_array_at(tokens, needed, choices_idx, 0);
        if (choice_idx >= 0 && tokens[choice_idx].type == JSTOK_OBJECT) {
            int text_idx = jstok_object_get(json, tokens, needed, choice_idx, "text");
            if (text_idx >= 0 && tokens[text_idx].type == JSTOK_STRING) {
                jstok_span_t sp = jstok_span(json, &tokens[text_idx]);
                if (text_delta) {
                    text_delta->ptr = sp.p;
                    text_delta->len = sp.n;
                }
            }

            int finish_idx = jstok_object_get(json, tokens, needed, choice_idx, "finish_reason");
            if (finish_idx >= 0 && tokens[finish_idx].type == JSTOK_STRING) {
                jstok_span_t sp = jstok_span(json, &tokens[finish_idx]);
                if (finish_reason) {
                    *finish_reason = llm_finish_reason_from_string(sp.p, sp.n);
                }
            }
        }
    }

    free(tokens);
    return 0;
}
