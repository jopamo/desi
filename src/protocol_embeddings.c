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

int parse_embeddings_response(const char* json, size_t len, llm_embeddings_result_t* result) {
    jstoktok_t* tokens = NULL;
    int count = 0;
    int ret = tokenize(json, len, &tokens, &count);
    if (ret < 0) return ret;

    if (count == 0 || tokens[0].type != JSTOK_OBJECT) {
        free_tokens(tokens);
        return LLM_PARSE_ERR_PROTOCOL;
    }

    memset(result, 0, sizeof(*result));

    int data_idx = obj_get_key(tokens, count, 0, json, "data");
    if (data_idx < 0 || tokens[data_idx].type != JSTOK_ARRAY || tokens[data_idx].size <= 0) {
        free_tokens(tokens);
        return LLM_PARSE_ERR_PROTOCOL;
    }

    size_t data_count = (size_t)tokens[data_idx].size;
    result->data = calloc(data_count, sizeof(llm_embedding_item_t));
    if (!result->data) {
        free_tokens(tokens);
        return JSTOK_ERROR_NOMEM;
    }
    result->data_count = data_count;

    for (size_t i = 0; i < data_count; i++) {
        int item_idx = arr_get(tokens, count, data_idx, (int)i);
        if (item_idx < 0 || tokens[item_idx].type != JSTOK_OBJECT) {
            free(result->data);
            result->data = NULL;
            result->data_count = 0;
            free_tokens(tokens);
            return LLM_PARSE_ERR_PROTOCOL;
        }
        int embedding_idx = obj_get_key(tokens, count, item_idx, json, "embedding");
        if (embedding_idx < 0 || tokens[embedding_idx].type != JSTOK_ARRAY) {
            free(result->data);
            result->data = NULL;
            result->data_count = 0;
            free_tokens(tokens);
            return LLM_PARSE_ERR_PROTOCOL;
        }
        span_t sp = tok_span(json, &tokens[embedding_idx]);
        result->data[i].embedding = sp.ptr;
        result->data[i].embedding_len = sp.len;
    }

    free_tokens(tokens);
    return 0;
}
