#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/json_core.h"
#include "llm/llm.h"

static void assert_true(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
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

static bool unescape_string_alloc(const char* json, const jstoktok_t* tok, char** out, size_t* out_len) {
    if (!json || !tok || tok->type != JSTOK_STRING) return false;
    jstok_span_t sp = jstok_span(json, tok);
    if (!sp.p || sp.n == 0) return false;
    size_t cap = sp.n;
    char* buf = malloc(cap + 1);
    if (!buf) return false;
    size_t unescaped_len = 0;
    if (jstok_unescape(json, tok, buf, cap, &unescaped_len) != 0) {
        free(buf);
        return false;
    }
    buf[unescaped_len] = '\0';
    *out = buf;
    *out_len = unescaped_len;
    return true;
}

static bool token_string_equals(const char* json, const jstoktok_t* tok, const char* expected, size_t expected_len) {
    char* unescaped = NULL;
    size_t unescaped_len = 0;
    if (!unescape_string_alloc(json, tok, &unescaped, &unescaped_len)) return false;
    bool ok = unescaped_len == expected_len && memcmp(unescaped, expected, expected_len) == 0;
    free(unescaped);
    return ok;
}

static bool expect_tool_call(const char* json, const jstoktok_t* tokens, int count, int tool_idx, const char* id,
                             size_t id_len, bool expect_id, const char* name, size_t name_len, const char* args_json,
                             size_t args_len) {
    if (tool_idx < 0 || tokens[tool_idx].type != JSTOK_OBJECT) return false;

    int type_idx = obj_get_key(tokens, count, tool_idx, json, "type");
    if (type_idx < 0 || tokens[type_idx].type != JSTOK_STRING) return false;
    if (!token_string_equals(json, &tokens[type_idx], "function", strlen("function"))) return false;

    int func_idx = obj_get_key(tokens, count, tool_idx, json, "function");
    if (func_idx < 0 || tokens[func_idx].type != JSTOK_OBJECT) return false;

    int name_idx = obj_get_key(tokens, count, func_idx, json, "name");
    if (name_idx < 0 || tokens[name_idx].type != JSTOK_STRING) return false;
    if (!token_string_equals(json, &tokens[name_idx], name, name_len)) return false;

    int args_idx = obj_get_key(tokens, count, func_idx, json, "arguments");
    if (args_idx < 0 || tokens[args_idx].type != JSTOK_STRING) return false;
    if (!token_string_equals(json, &tokens[args_idx], args_json, args_len)) return false;

    int id_idx = obj_get_key(tokens, count, tool_idx, json, "id");
    if (expect_id) {
        if (id_idx < 0 || tokens[id_idx].type != JSTOK_STRING) return false;
        if (!token_string_equals(json, &tokens[id_idx], id, id_len)) return false;
    } else if (id_idx >= 0) {
        return false;
    }

    return true;
}

static bool test_tool_calls_build_basic(void) {
    const char* args0 = "{\"x\":1}";
    const char* args1 = "{\"note\":\"hi\"}";
    llm_tool_call_build_t calls[2];
    calls[0].id = "call_1";
    calls[0].id_len = 6;
    calls[0].name = "add";
    calls[0].name_len = 3;
    calls[0].arguments_json = args0;
    calls[0].arguments_json_len = strlen(args0);
    calls[1].id = NULL;
    calls[1].id_len = 0;
    calls[1].name = "echo";
    calls[1].name_len = 4;
    calls[1].arguments_json = args1;
    calls[1].arguments_json_len = strlen(args1);

    char out[512];
    size_t out_len = 0;
    bool ok = llm_tool_calls_json_write(calls, 2, out, sizeof(out) - 1, 64, &out_len);
    assert_true(ok, "builder failed");
    assert_true(out_len > 0, "builder length");
    out[out_len] = '\0';

    const char* expected =
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"add\",\"arguments\":\"{\\\"x\\\":1}\"}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"echo\",\"arguments\":\"{\\\"note\\\":\\\"hi\\\"}\"}}]";
    assert_true(out_len == strlen(expected) && memcmp(out, expected, out_len) == 0, "deterministic output");

    jstoktok_t* tokens = NULL;
    int count = 0;
    assert_true(parse_tokens(out, out_len, &tokens, &count), "parse tool_calls json");
    assert_true(count > 0 && tokens[0].type == JSTOK_ARRAY, "root array");
    assert_true(tokens[0].size == 2, "tool_calls size");

    int first_idx = arr_get(tokens, count, 0, 0);
    int second_idx = arr_get(tokens, count, 0, 1);
    assert_true(expect_tool_call(out, tokens, count, first_idx, "call_1", 6, true, "add", 3, args0, strlen(args0)),
                "first tool call fields");
    assert_true(expect_tool_call(out, tokens, count, second_idx, NULL, 0, false, "echo", 4, args1, strlen(args1)),
                "second tool call fields");

    free(tokens);
    return true;
}

static bool test_tool_calls_build_empty(void) {
    char out[16];
    size_t out_len = 0;
    bool ok = llm_tool_calls_json_write(NULL, 0, out, sizeof(out) - 1, 0, &out_len);
    assert_true(ok, "empty tool_calls build failed");
    out[out_len] = '\0';
    assert_true(strcmp(out, "[]") == 0, "empty tool_calls output");

    jstoktok_t* tokens = NULL;
    int count = 0;
    assert_true(parse_tokens(out, out_len, &tokens, &count), "parse empty tool_calls");
    assert_true(count > 0 && tokens[0].type == JSTOK_ARRAY, "empty root array");
    assert_true(tokens[0].size == 0, "empty tool_calls size");
    free(tokens);
    return true;
}

static bool test_tool_calls_build_limits(void) {
    const char* args = "{\"x\":1}";
    llm_tool_call_build_t call;
    call.id = "call_1";
    call.id_len = 6;
    call.name = "add";
    call.name_len = 3;
    call.arguments_json = args;
    call.arguments_json_len = strlen(args);

    char out[64];
    size_t out_len = 99;
    bool ok = llm_tool_calls_json_write(&call, 1, out, sizeof(out), 4, &out_len);
    assert_true(!ok, "args cap not enforced");
    assert_true(out_len == 0, "args cap length");

    ok = llm_tool_calls_json_write(&call, 1, out, 10, 0, &out_len);
    assert_true(!ok, "output cap not enforced");
    assert_true(out_len == 0, "output cap length");
    return true;
}

int main(void) {
    assert_true(test_tool_calls_build_basic(), "basic tool_calls build failed");
    assert_true(test_tool_calls_build_empty(), "empty tool_calls build failed");
    assert_true(test_tool_calls_build_limits(), "tool_calls build limits failed");
    printf("Tool call build tests passed.\n");
    return 0;
}
