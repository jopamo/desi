#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/json_core.h"
#include "llm/llm.h"

static bool require(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return false;
    }
    return true;
}

static bool parse_tokens(const char* json, size_t len, jstoktok_t** tokens_out, int* count_out) {
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return false;
    jstoktok_t* tokens = malloc((size_t)needed * sizeof(*tokens));
    if (!tokens) return false;
    jstok_init(&parser);
    int parsed = jstok_parse(&parser, json, (int)len, tokens, needed);
    if (parsed <= 0) {
        free(tokens);
        return false;
    }
    *tokens_out = tokens;
    *count_out = parsed;
    return true;
}

static bool token_string_equals(const char* json, const jstoktok_t* tok, const char* expected, size_t expected_len) {
    if (!json || !tok || tok->type != JSTOK_STRING) return false;
    jstok_span_t sp = jstok_span(json, tok);
    if (!sp.p) return false;
    char* buf = malloc(sp.n + 1);
    if (!buf) return false;
    size_t unescaped_len = 0;
    if (jstok_unescape(json, tok, buf, sp.n, &unescaped_len) != 0) {
        free(buf);
        return false;
    }
    bool ok = unescaped_len == expected_len && memcmp(buf, expected, expected_len) == 0;
    free(buf);
    return ok;
}

static bool expect_primitive(const char* json, const jstoktok_t* tokens, int count, const char* key,
                             const char* expected) {
    int idx = obj_get_key(tokens, count, 0, json, key);
    if (idx < 0 || tokens[idx].type != JSTOK_PRIMITIVE) return false;
    span_t sp = tok_span(json, &tokens[idx]);
    size_t expected_len = strlen(expected);
    return sp.len == expected_len && memcmp(sp.ptr, expected, expected_len) == 0;
}

static bool expect_stop_array(const char* json, const jstoktok_t* tokens, int count, const char* const* expected,
                              const size_t* expected_lens, size_t expected_count) {
    int idx = obj_get_key(tokens, count, 0, json, "stop");
    if (idx < 0 || tokens[idx].type != JSTOK_ARRAY) return false;
    if (tokens[idx].size != (int)expected_count) return false;
    for (size_t i = 0; i < expected_count; i++) {
        int elem_idx = arr_get(tokens, count, idx, (int)i);
        if (elem_idx < 0 || tokens[elem_idx].type != JSTOK_STRING) return false;
        if (!token_string_equals(json, &tokens[elem_idx], expected[i], expected_lens[i])) return false;
    }
    return true;
}

static bool expect_stop_string(const char* json, const jstoktok_t* tokens, int count, const char* expected,
                               size_t expected_len) {
    int idx = obj_get_key(tokens, count, 0, json, "stop");
    if (idx < 0 || tokens[idx].type != JSTOK_STRING) return false;
    return token_string_equals(json, &tokens[idx], expected, expected_len);
}

static bool test_request_opts_basic(void) {
    llm_request_opts_t opts = {0};
    opts.has_temperature = true;
    opts.temperature = 0.5;
    opts.has_top_p = true;
    opts.top_p = 0.25;
    opts.has_max_tokens = true;
    opts.max_tokens = 256;
    const char* stops[] = {"a", "bb"};
    size_t stop_lens[] = {1, 2};
    opts.stop_list = stops;
    opts.stop_lens = stop_lens;
    opts.stop_count = 2;
    opts.has_frequency_penalty = true;
    opts.frequency_penalty = -0.5;
    opts.has_presence_penalty = true;
    opts.presence_penalty = 0.75;
    opts.has_seed = true;
    opts.seed = 42;

    char out[512];
    size_t out_len = 0;
    bool ok = llm_request_opts_json_write(&opts, out, sizeof(out), 4, 8, &out_len);
    if (!require(ok, "request opts write")) return false;
    out[out_len] = '\0';

    const char* expected =
        "{\"temperature\":0.5,\"top_p\":0.25,\"max_tokens\":256,\"stop\":[\"a\",\"bb\"],\"frequency_penalty\":-0.5,"
        "\"presence_penalty\":0.75,\"seed\":42}";
    if (!require(strcmp(out, expected) == 0, "deterministic output")) return false;

    jstoktok_t* tokens = NULL;
    int count = 0;
    if (!require(parse_tokens(out, out_len, &tokens, &count), "parse request opts")) return false;
    if (!require(count > 0 && tokens[0].type == JSTOK_OBJECT, "root object")) {
        free(tokens);
        return false;
    }
    if (!require(expect_primitive(out, tokens, count, "temperature", "0.5"), "temperature value")) {
        free(tokens);
        return false;
    }
    if (!require(expect_primitive(out, tokens, count, "top_p", "0.25"), "top_p value")) {
        free(tokens);
        return false;
    }
    if (!require(expect_primitive(out, tokens, count, "max_tokens", "256"), "max_tokens value")) {
        free(tokens);
        return false;
    }
    if (!require(expect_stop_array(out, tokens, count, stops, stop_lens, 2), "stop array")) {
        free(tokens);
        return false;
    }
    if (!require(expect_primitive(out, tokens, count, "frequency_penalty", "-0.5"), "frequency_penalty value")) {
        free(tokens);
        return false;
    }
    if (!require(expect_primitive(out, tokens, count, "presence_penalty", "0.75"), "presence_penalty value")) {
        free(tokens);
        return false;
    }
    if (!require(expect_primitive(out, tokens, count, "seed", "42"), "seed value")) {
        free(tokens);
        return false;
    }
    free(tokens);
    return true;
}

static bool test_request_opts_single_stop(void) {
    llm_request_opts_t opts = {0};
    opts.stop = "DONE";
    opts.stop_len = 4;

    char out[128];
    size_t out_len = 0;
    bool ok = llm_request_opts_json_write(&opts, out, sizeof(out), 1, 8, &out_len);
    if (!require(ok, "single stop write")) return false;
    out[out_len] = '\0';

    jstoktok_t* tokens = NULL;
    int count = 0;
    if (!require(parse_tokens(out, out_len, &tokens, &count), "parse single stop")) return false;
    if (!require(count > 0 && tokens[0].type == JSTOK_OBJECT, "root object single stop")) {
        free(tokens);
        return false;
    }
    if (!require(expect_stop_string(out, tokens, count, "DONE", 4), "stop string")) {
        free(tokens);
        return false;
    }
    free(tokens);
    return true;
}

static bool test_request_opts_empty(void) {
    llm_request_opts_t opts = {0};
    char out[8];
    size_t out_len = 1;
    bool ok = llm_request_opts_json_write(&opts, out, sizeof(out), 0, 0, &out_len);
    if (!require(ok, "empty opts write")) return false;
    if (!require(out_len == 0, "empty opts length")) return false;
    if (!require(out[0] == '\0', "empty opts string")) return false;
    return true;
}

static bool test_request_opts_bounds(void) {
    llm_request_opts_t opts = {0};
    const char* stops[] = {"a", "bb"};
    size_t stop_lens[] = {1, 2};
    opts.stop_list = stops;
    opts.stop_lens = stop_lens;
    opts.stop_count = 2;

    char out[128];
    size_t out_len = 0;
    bool ok = llm_request_opts_json_write(&opts, out, sizeof(out), 1, 8, &out_len);
    if (!require(!ok, "max stop strings enforced")) return false;

    ok = llm_request_opts_json_write(&opts, out, sizeof(out), 0, 2, &out_len);
    if (!require(!ok, "max stop bytes enforced")) return false;

    return true;
}

static bool test_request_opts_nan_inf(void) {
    llm_request_opts_t opts = {0};
    char out[64];
    size_t out_len = 0;

    opts.has_temperature = true;
    opts.temperature = NAN;
    bool ok = llm_request_opts_json_write(&opts, out, sizeof(out), 0, 0, &out_len);
    if (!require(!ok, "reject NaN")) return false;

    opts.has_temperature = false;
    opts.has_top_p = true;
    opts.top_p = INFINITY;
    ok = llm_request_opts_json_write(&opts, out, sizeof(out), 0, 0, &out_len);
    if (!require(!ok, "reject infinity")) return false;

    return true;
}

int main(void) {
    if (!test_request_opts_basic()) return 1;
    if (!test_request_opts_single_stop()) return 1;
    if (!test_request_opts_empty()) return 1;
    if (!test_request_opts_bounds()) return 1;
    if (!test_request_opts_nan_inf()) return 1;
    printf("Request opts tests passed.\n");
    return 0;
}
