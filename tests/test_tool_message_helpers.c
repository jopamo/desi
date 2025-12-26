#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/json_core.h"
#include "llm/llm.h"
#include "src/json_build.h"

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

static bool expect_tool_message(const char* json, size_t len, const char* content, size_t content_len,
                                const char* tool_call_id, size_t tool_call_id_len, const char* name, size_t name_len,
                                bool expect_name) {
    jstoktok_t* tokens = NULL;
    int count = 0;
    bool ok = false;

    if (!parse_tokens(json, len, &tokens, &count)) return false;
    if (count <= 0 || tokens[0].type != JSTOK_OBJECT) goto cleanup;

    int messages_idx = obj_get_key(tokens, count, 0, json, "messages");
    if (messages_idx < 0 || tokens[messages_idx].type != JSTOK_ARRAY) goto cleanup;
    if (tokens[messages_idx].size < 1) goto cleanup;

    int msg_idx = arr_get(tokens, count, messages_idx, 0);
    if (msg_idx < 0 || tokens[msg_idx].type != JSTOK_OBJECT) goto cleanup;

    int role_idx = obj_get_key(tokens, count, msg_idx, json, "role");
    if (role_idx < 0 || tokens[role_idx].type != JSTOK_STRING) goto cleanup;
    if (!token_string_equals(json, &tokens[role_idx], "tool", 4)) goto cleanup;

    int content_idx = obj_get_key(tokens, count, msg_idx, json, "content");
    if (content_idx < 0 || tokens[content_idx].type != JSTOK_STRING) goto cleanup;
    if (!token_string_equals(json, &tokens[content_idx], content, content_len)) goto cleanup;

    int id_idx = obj_get_key(tokens, count, msg_idx, json, "tool_call_id");
    if (id_idx < 0 || tokens[id_idx].type != JSTOK_STRING) goto cleanup;
    if (!token_string_equals(json, &tokens[id_idx], tool_call_id, tool_call_id_len)) goto cleanup;

    int name_idx = obj_get_key(tokens, count, msg_idx, json, "name");
    if (expect_name) {
        if (name_idx < 0 || tokens[name_idx].type != JSTOK_STRING) goto cleanup;
        if (!token_string_equals(json, &tokens[name_idx], name, name_len)) goto cleanup;
    } else if (name_idx >= 0) {
        goto cleanup;
    }

    ok = true;

cleanup:
    free(tokens);
    return ok;
}

static bool test_tool_message_basic(void) {
    const char* content = "ok";
    const char* tool_call_id = "call_1";
    llm_message_t msg;
    bool ok = llm_tool_message_init(&msg, content, strlen(content), tool_call_id, strlen(tool_call_id), NULL, 0);
    assert_true(ok, "tool message init failed");

    char* json = build_chat_request("test-model", &msg, 1, false, false, NULL, NULL, NULL);
    assert_true(json != NULL, "build_chat_request failed");
    ok = expect_tool_message(json, strlen(json), content, strlen(content), tool_call_id, strlen(tool_call_id), NULL, 0,
                             false);
    free(json);
    assert_true(ok, "tool message fields mismatch");
    return true;
}

static bool test_tool_message_with_name(void) {
    const char* content = "{\"note\":\"hi\"}";
    const char* tool_call_id = "call_2";
    const char* name = "add";
    llm_message_t msg;
    bool ok =
        llm_tool_message_init(&msg, content, strlen(content), tool_call_id, strlen(tool_call_id), name, strlen(name));
    assert_true(ok, "tool message init with name failed");

    char* json = build_chat_request("test-model", &msg, 1, false, false, NULL, NULL, NULL);
    assert_true(json != NULL, "build_chat_request failed");
    ok = expect_tool_message(json, strlen(json), content, strlen(content), tool_call_id, strlen(tool_call_id), name,
                             strlen(name), true);
    free(json);
    assert_true(ok, "tool message with name fields mismatch");
    return true;
}

static bool test_tool_message_invalid(void) {
    llm_message_t msg;
    bool ok = llm_tool_message_init(&msg, "ok", 2, NULL, 0, NULL, 0);
    assert_true(!ok, "missing tool_call_id should fail");
    ok = llm_tool_message_init(&msg, "ok", 2, "call_1", 6, "tool", 0);
    assert_true(!ok, "zero tool name length should fail");
    ok = llm_tool_message_init(&msg, "ok", 2, "call_1", 6, NULL, 3);
    assert_true(!ok, "tool name length without pointer should fail");
    ok = llm_tool_message_init(&msg, NULL, 2, "call_1", 6, NULL, 0);
    assert_true(!ok, "content length without pointer should fail");
    return true;
}

int main(void) {
    assert_true(test_tool_message_basic(), "basic tool message test failed");
    assert_true(test_tool_message_with_name(), "tool message with name test failed");
    assert_true(test_tool_message_invalid(), "tool message invalid test failed");
    printf("Tool message helper tests passed.\n");
    return 0;
}
