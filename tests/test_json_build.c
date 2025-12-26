#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/json_core.h"
#include "src/json_build.h"

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
    size_t cap = sp.n + 1;
    char* buf = malloc(cap);
    if (!buf) return false;
    size_t out_len = 0;
    if (jstok_unescape(json, tok, buf, cap, &out_len) != 0) {
        free(buf);
        return false;
    }
    bool ok = out_len == expected_len && memcmp(buf, expected, expected_len) == 0;
    free(buf);
    return ok;
}

static bool token_string_equals_lit(const char* json, const jstoktok_t* tok, const char* expected) {
    return token_string_equals(json, tok, expected, strlen(expected));
}

int main(void) {
    llm_message_t messages[] = {{LLM_ROLE_SYSTEM, "You are a helpful assistant.",
                                 strlen("You are a helpful assistant."), NULL, 0, NULL, 0, NULL, 0, NULL, 0},
                                {LLM_ROLE_USER, "Hello!", strlen("Hello!"), NULL, 0, NULL, 0, NULL, 0, NULL, 0}};

    char* json = build_chat_request("gpt-4o", messages, 2, false, false, "{\"temperature\":0.7}", NULL, NULL, 0, 0);
    if (!json) {
        fprintf(stderr, "build_chat_request failed\n");
        return 1;
    }

    jstoktok_t* tokens = NULL;
    int count = 0;
    if (!parse_tokens(json, strlen(json), &tokens, &count)) {
        fprintf(stderr, "FAIL: parse\n");
        free(json);
        return 1;
    }

    int model_idx = jstok_object_get(json, tokens, count, 0, "model");
    if (!require(model_idx >= 0 && token_string_equals_lit(json, &tokens[model_idx], "gpt-4o"), "model")) {
        free(tokens);
        free(json);
        return 1;
    }

    int messages_idx = jstok_object_get(json, tokens, count, 0, "messages");
    if (!require(messages_idx >= 0 && tokens[messages_idx].type == JSTOK_ARRAY, "messages array")) {
        free(tokens);
        free(json);
        return 1;
    }
    if (!require(tokens[messages_idx].size == 2, "messages count")) {
        free(tokens);
        free(json);
        return 1;
    }

    int msg0 = jstok_array_at(tokens, count, messages_idx, 0);
    int msg1 = jstok_array_at(tokens, count, messages_idx, 1);
    if (!require(msg0 >= 0 && msg1 >= 0, "message indices")) {
        free(tokens);
        free(json);
        return 1;
    }

    int role0 = jstok_object_get(json, tokens, count, msg0, "role");
    int content0 = jstok_object_get(json, tokens, count, msg0, "content");
    if (!require(role0 >= 0 && content0 >= 0, "msg0 fields")) {
        free(tokens);
        free(json);
        return 1;
    }
    if (!require(token_string_equals_lit(json, &tokens[role0], "system"), "msg0 role")) {
        free(tokens);
        free(json);
        return 1;
    }
    if (!require(token_string_equals_lit(json, &tokens[content0], "You are a helpful assistant."), "msg0 content")) {
        free(tokens);
        free(json);
        return 1;
    }

    int role1 = jstok_object_get(json, tokens, count, msg1, "role");
    int content1 = jstok_object_get(json, tokens, count, msg1, "content");
    if (!require(role1 >= 0 && content1 >= 0, "msg1 fields")) {
        free(tokens);
        free(json);
        return 1;
    }
    if (!require(token_string_equals_lit(json, &tokens[role1], "user"), "msg1 role")) {
        free(tokens);
        free(json);
        return 1;
    }
    if (!require(token_string_equals_lit(json, &tokens[content1], "Hello!"), "msg1 content")) {
        free(tokens);
        free(json);
        return 1;
    }

    int temp_idx = jstok_object_get(json, tokens, count, 0, "temperature");
    if (!require(temp_idx >= 0 && tokens[temp_idx].type == JSTOK_PRIMITIVE, "temperature")) {
        free(tokens);
        free(json);
        return 1;
    }
    jstok_span_t temp_sp = jstok_span(json, &tokens[temp_idx]);
    if (!require(temp_sp.p && temp_sp.n == 3 && memcmp(temp_sp.p, "0.7", 3) == 0, "temperature value")) {
        free(tokens);
        free(json);
        return 1;
    }

    free(tokens);
    free(json);

    const char* esc_content = "Quotes: \" and Backslash: \\";
    llm_message_t msg_esc[] = {{LLM_ROLE_USER, esc_content, strlen(esc_content), NULL, 0, NULL, 0, NULL, 0, NULL, 0}};
    json = build_chat_request("gpt-4o", msg_esc, 1, false, false, NULL, NULL, NULL, 0, 0);
    if (!json) {
        fprintf(stderr, "FAIL: escape build\n");
        return 1;
    }
    tokens = NULL;
    count = 0;
    if (!parse_tokens(json, strlen(json), &tokens, &count)) {
        fprintf(stderr, "FAIL: escape parse\n");
        free(json);
        return 1;
    }
    int esc_messages = jstok_object_get(json, tokens, count, 0, "messages");
    int esc_msg = jstok_array_at(tokens, count, esc_messages, 0);
    int esc_content_idx = jstok_object_get(json, tokens, count, esc_msg, "content");
    if (!require(esc_content_idx >= 0, "escaped content token")) {
        free(tokens);
        free(json);
        return 1;
    }
    if (!require(token_string_equals(json, &tokens[esc_content_idx], esc_content, strlen(esc_content)),
                 "escaped content value")) {
        free(tokens);
        free(json);
        return 1;
    }
    free(tokens);
    free(json);

    const char* parts_json = "[{\"type\":\"text\",\"text\":\"hello\"},{\"type\":\"text\",\"text\":\"world\"}]";
    llm_message_t parts_msg = {.role = LLM_ROLE_USER,
                               .content = NULL,
                               .content_len = 0,
                               .content_json = parts_json,
                               .content_json_len = strlen(parts_json)};
    json = build_chat_request("gpt-4o", &parts_msg, 1, false, false, NULL, NULL, NULL, 2, 1024);
    if (!json) {
        fprintf(stderr, "FAIL: parts build\n");
        return 1;
    }
    tokens = NULL;
    count = 0;
    if (!parse_tokens(json, strlen(json), &tokens, &count)) {
        fprintf(stderr, "FAIL: parts parse\n");
        free(json);
        return 1;
    }
    int parts_messages = jstok_object_get(json, tokens, count, 0, "messages");
    int parts_msg_idx = jstok_array_at(tokens, count, parts_messages, 0);
    int content_idx = jstok_object_get(json, tokens, count, parts_msg_idx, "content");
    if (!require(content_idx >= 0 && tokens[content_idx].type == JSTOK_ARRAY, "parts content array")) {
        free(tokens);
        free(json);
        return 1;
    }
    if (!require(tokens[content_idx].size == 2, "parts content size")) {
        free(tokens);
        free(json);
        return 1;
    }
    int part0 = jstok_array_at(tokens, count, content_idx, 0);
    int part1 = jstok_array_at(tokens, count, content_idx, 1);
    int part0_type = jstok_object_get(json, tokens, count, part0, "type");
    int part0_text = jstok_object_get(json, tokens, count, part0, "text");
    int part1_text = jstok_object_get(json, tokens, count, part1, "text");
    if (!require(part0_type >= 0 && part0_text >= 0 && part1_text >= 0, "parts fields")) {
        free(tokens);
        free(json);
        return 1;
    }
    if (!require(token_string_equals_lit(json, &tokens[part0_type], "text"), "part0 type")) {
        free(tokens);
        free(json);
        return 1;
    }
    if (!require(token_string_equals_lit(json, &tokens[part0_text], "hello"), "part0 text")) {
        free(tokens);
        free(json);
        return 1;
    }
    if (!require(token_string_equals_lit(json, &tokens[part1_text], "world"), "part1 text")) {
        free(tokens);
        free(json);
        return 1;
    }
    free(tokens);
    free(json);

    json = build_chat_request("gpt-4o", &parts_msg, 1, false, false, NULL, NULL, NULL, 1, 1024);
    if (!require(json == NULL, "parts count limit")) return 1;

    json = build_chat_request("gpt-4o", &parts_msg, 1, false, false, NULL, NULL, NULL, 2, 4);
    if (!require(json == NULL, "parts byte limit")) return 1;

    const char* invalid_parts = "{\"type\":\"text\"}";
    llm_message_t invalid_msg = {
        .role = LLM_ROLE_USER, .content_json = invalid_parts, .content_json_len = strlen(invalid_parts)};
    json = build_chat_request("gpt-4o", &invalid_msg, 1, false, false, NULL, NULL, NULL, 2, 1024);
    if (!require(json == NULL, "invalid content json")) return 1;

    llm_message_t both_msg = {.role = LLM_ROLE_USER,
                              .content = "hi",
                              .content_len = 2,
                              .content_json = parts_json,
                              .content_json_len = strlen(parts_json)};
    json = build_chat_request("gpt-4o", &both_msg, 1, false, false, NULL, NULL, NULL, 2, 1024);
    if (!require(json == NULL, "content and content_json exclusive")) return 1;

    printf("JSON build test passed!\n");
    return 0;
}
