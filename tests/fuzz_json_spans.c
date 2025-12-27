#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "llm/json_core.h"

enum { FUZZ_MAX_INPUT = 4096, FUZZ_MAX_TOKENS = 256, KEY_BUF_CAP = 32 };

static size_t build_key(char* out, size_t cap, const uint8_t* data, size_t size) {
    if (cap == 0) return 0;
    size_t limit = cap - 1;
    size_t key_len = limit ? 1 + (data[0] % limit) : 0;
    if (key_len > limit) key_len = limit;
    for (size_t i = 0; i < key_len; i++) {
        uint8_t b = (i + 1 < size) ? data[i + 1] : (uint8_t)i;
        out[i] = (char)('a' + (b % 26));
    }
    out[key_len] = '\0';
    return key_len;
}

static void assert_span_bounds(const char* json, size_t len, span_t sp) {
    if (!sp.ptr) {
        if (sp.len != 0) abort();
        return;
    }
    const char* end = json + len;
    if (sp.ptr < json || sp.ptr > end) abort();
    if (sp.len > (size_t)(end - sp.ptr)) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data) return 0;
    if (size > FUZZ_MAX_INPUT) size = FUZZ_MAX_INPUT;
    if (size == 0) return 0;

    const char* json = (const char*)data;
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)size, NULL, 0);
    if (needed <= 0 || needed > FUZZ_MAX_TOKENS) return 0;

    jstoktok_t* tokens = malloc((size_t)needed * sizeof(*tokens));
    if (!tokens) return 0;

    jstok_init(&parser);
    int count = jstok_parse(&parser, json, (int)size, tokens, needed);
    if (count <= 0) {
        free(tokens);
        return 0;
    }

    char key_buf[KEY_BUF_CAP];
    build_key(key_buf, sizeof(key_buf), data, size);

    for (int i = 0; i < count; i++) {
        span_t sp = tok_span(json, &tokens[i]);
        assert_span_bounds(json, size, sp);
        (void)tok_is_type(&tokens[i], tokens[i].type);

        if (tokens[i].type == JSTOK_STRING) {
            (void)tok_eq_lit(json, &tokens[i], key_buf);
        }

        if (tokens[i].type == JSTOK_OBJECT) {
            int idx = obj_get_key(tokens, count, i, json, key_buf);
            if (idx >= 0) {
                span_t v = tok_span(json, &tokens[idx]);
                assert_span_bounds(json, size, v);
            }
        }

        if (tokens[i].type == JSTOK_ARRAY && tokens[i].size > 0) {
            int seed = (int)data[(size_t)i % size];
            int arr_idx = seed % tokens[i].size;
            int idx = arr_get(tokens, count, i, arr_idx);
            if (idx >= 0) {
                span_t v = tok_span(json, &tokens[idx]);
                assert_span_bounds(json, size, v);
            }
        }

        int next = skip_subtree(tokens, count, i);
        if (next < i || next > count) {
            free(tokens);
            abort();
        }
    }

    free(tokens);
    return 0;
}
