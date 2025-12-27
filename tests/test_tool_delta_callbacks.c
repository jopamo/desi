#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSTOK_HEADER
#include <jstok.h>

#include "fake_transport.h"
#include "llm/llm.h"
#include "transport_curl.h"

static bool require(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return false;
    }
    return true;
}

static fake_transport_state_t* g_fake;

static void fake_reset(void) {
    fake_transport_reset();
    g_fake = fake_transport_state();
}

struct tool_delta_capture {
    size_t calls;
    size_t args_calls;
    size_t indices[4];
    size_t missing_id;
    size_t missing_name;
    char id[32];
    size_t id_len;
    char name[32];
    size_t name_len;
    char args[256];
    size_t args_len;
    char args_frag[256];
    size_t args_frag_len;
    size_t final_calls;
    size_t final_indices[4];
    char final_args[256];
    size_t final_args_len;
    bool overflow;
};

struct tool_multi_capture {
    size_t delta_calls;
    size_t fragment_calls[2];
    char raw_args[2][256];
    size_t raw_args_len[2];
    char id[2][32];
    size_t id_len[2];
    char name[2][32];
    size_t name_len[2];
    size_t final_calls;
    size_t final_indices[4];
    char final_args[2][256];
    size_t final_args_len[2];
    bool overflow;
};

static bool append_fragment(char* buf, size_t cap, size_t* len, const char* data, size_t data_len) {
    if (*len + data_len >= cap) return false;
    memcpy(buf + *len, data, data_len);
    *len += data_len;
    buf[*len] = '\0';
    return true;
}

static void on_tool_delta(void* user_data, const llm_tool_call_delta_t* delta) {
    struct tool_delta_capture* cap = user_data;
    if (cap->calls < 4) {
        cap->indices[cap->calls] = delta->index;
    }
    cap->calls++;

    if (delta->id) {
        size_t take = delta->id_len < sizeof(cap->id) ? delta->id_len : sizeof(cap->id) - 1;
        memcpy(cap->id, delta->id, take);
        cap->id[take] = '\0';
        cap->id_len = take;
    } else {
        cap->missing_id++;
    }

    if (delta->name) {
        size_t take = delta->name_len < sizeof(cap->name) ? delta->name_len : sizeof(cap->name) - 1;
        memcpy(cap->name, delta->name, take);
        cap->name[take] = '\0';
        cap->name_len = take;
    } else {
        cap->missing_name++;
    }

    if (delta->arguments_fragment) {
        if (!append_fragment(cap->args, sizeof(cap->args), &cap->args_len, delta->arguments_fragment,
                             delta->arguments_fragment_len)) {
            cap->overflow = true;
        }
    }
}

static void on_tool_args_fragment(void* user_data, size_t tool_index, const char* fragment, size_t len) {
    struct tool_delta_capture* cap = user_data;
    (void)tool_index;
    cap->args_calls++;
    if (!append_fragment(cap->args_frag, sizeof(cap->args_frag), &cap->args_frag_len, fragment, len)) {
        cap->overflow = true;
    }
}

static void on_tool_args_complete(void* user_data, size_t tool_index, const char* args_json, size_t len) {
    struct tool_delta_capture* cap = user_data;
    if (cap->final_calls < 4) {
        cap->final_indices[cap->final_calls] = tool_index;
    }
    cap->final_calls++;
    if (!append_fragment(cap->final_args, sizeof(cap->final_args), &cap->final_args_len, args_json, len)) {
        cap->overflow = true;
    }
}

static void on_tool_delta_multi(void* user_data, const llm_tool_call_delta_t* delta) {
    struct tool_multi_capture* cap = user_data;
    cap->delta_calls++;
    if (delta->index >= 2) {
        cap->overflow = true;
        return;
    }
    size_t idx = delta->index;

    if (delta->id && cap->id_len[idx] == 0) {
        size_t take = delta->id_len < sizeof(cap->id[idx]) ? delta->id_len : sizeof(cap->id[idx]) - 1;
        memcpy(cap->id[idx], delta->id, take);
        cap->id[idx][take] = '\0';
        cap->id_len[idx] = take;
    }
    if (delta->name && cap->name_len[idx] == 0) {
        size_t take = delta->name_len < sizeof(cap->name[idx]) ? delta->name_len : sizeof(cap->name[idx]) - 1;
        memcpy(cap->name[idx], delta->name, take);
        cap->name[idx][take] = '\0';
        cap->name_len[idx] = take;
    }
    if (delta->arguments_fragment) {
        cap->fragment_calls[idx]++;
        if (!append_fragment(cap->raw_args[idx], sizeof(cap->raw_args[idx]), &cap->raw_args_len[idx],
                             delta->arguments_fragment, delta->arguments_fragment_len)) {
            cap->overflow = true;
        }
    }
}

static void on_tool_args_complete_multi(void* user_data, size_t tool_index, const char* args_json, size_t len) {
    struct tool_multi_capture* cap = user_data;
    if (cap->final_calls < 4) {
        cap->final_indices[cap->final_calls] = tool_index;
    }
    cap->final_calls++;
    if (tool_index >= 2) {
        cap->overflow = true;
        return;
    }
    size_t take = len < sizeof(cap->final_args[tool_index]) ? len : sizeof(cap->final_args[tool_index]) - 1;
    memcpy(cap->final_args[tool_index], args_json, take);
    cap->final_args[tool_index][take] = '\0';
    cap->final_args_len[tool_index] = take;
}

static bool parse_json_tokens(const char* json, size_t len, jstoktok_t** tokens_out, int* count_out) {
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return false;

    jstoktok_t* tokens = malloc((size_t)needed * sizeof(jstoktok_t));
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

static bool json_unescape_alloc(const char* escaped, size_t escaped_len, char** out, size_t* out_len) {
    size_t tmp_len = escaped_len + 2;
    char* tmp = malloc(tmp_len + 1);
    if (!tmp) return false;
    tmp[0] = '"';
    memcpy(tmp + 1, escaped, escaped_len);
    tmp[1 + escaped_len] = '"';
    tmp[tmp_len] = '\0';

    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, tmp, (int)tmp_len, NULL, 0);
    if (needed <= 0) {
        free(tmp);
        return false;
    }

    jstoktok_t* tokens = malloc((size_t)needed * sizeof(jstoktok_t));
    if (!tokens) {
        free(tmp);
        return false;
    }

    jstok_init(&parser);
    int parsed = jstok_parse(&parser, tmp, (int)tmp_len, tokens, needed);
    if (parsed <= 0 || tokens[0].type != JSTOK_STRING) {
        free(tokens);
        free(tmp);
        return false;
    }

    size_t cap = escaped_len + 1;
    char* unescaped = malloc(cap + 1);
    if (!unescaped) {
        free(tokens);
        free(tmp);
        return false;
    }

    size_t unescaped_len = 0;
    if (jstok_unescape(tmp, &tokens[0], unescaped, cap, &unescaped_len) != 0) {
        free(unescaped);
        free(tokens);
        free(tmp);
        return false;
    }
    unescaped[unescaped_len] = '\0';

    free(tokens);
    free(tmp);

    *out = unescaped;
    *out_len = unescaped_len;
    return true;
}

static bool json_expect_number(const char* json, const jstoktok_t* tokens, int count, int obj_idx, const char* key,
                               long long expected) {
    int idx = jstok_object_get(json, tokens, count, obj_idx, key);
    if (idx < 0 || tokens[idx].type != JSTOK_PRIMITIVE) return false;
    long long value = 0;
    if (jstok_atoi64(json, &tokens[idx], &value) != 0) return false;
    return value == expected;
}

static bool json_expect_string(const char* json, const jstoktok_t* tokens, int count, int obj_idx, const char* key,
                               const char* expected) {
    int idx = jstok_object_get(json, tokens, count, obj_idx, key);
    if (idx < 0 || tokens[idx].type != JSTOK_STRING) return false;
    jstok_span_t sp = jstok_span(json, &tokens[idx]);
    size_t expected_len = strlen(expected);
    return sp.n == expected_len && memcmp(sp.p, expected, expected_len) == 0;
}

int main(void) {
    fake_reset();

    llm_model_t model = {"test-model"};
    llm_client_t* client = llm_client_create("http://fake", &model, NULL, NULL);
    if (!require(client != NULL, "client create")) return 1;

    const char* response_post =
        "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"tool_calls\":[{\"id\":\"call_0\","
        "\"function\":{\"name\":\"ping\",\"arguments\":\"{\\\"a\\\":1,\\\"note\\\":\\\"hi\\\\nthere\\\"}\"}}]}}]}";
    g_fake->response_post = response_post;

    llm_message_t messages[] = {{LLM_ROLE_USER, "ping", 4, NULL, 0, NULL, 0, NULL, 0, NULL, 0}};
    llm_chat_result_t result;
    if (!require(llm_chat(client, messages, 1, NULL, NULL, NULL, &result), "non-stream chat")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(g_fake->called_post, "transport not called")) {
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(result.finish_reason == LLM_FINISH_REASON_TOOL_CALLS, "finish_reason tool_calls")) {
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(result.tool_calls_count == 1, "tool_calls count")) {
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(result.tool_calls[0].id_len == 6 && memcmp(result.tool_calls[0].id, "call_0", 6) == 0, "tool id")) {
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(result.tool_calls[0].name_len == 4 && memcmp(result.tool_calls[0].name, "ping", 4) == 0,
                 "tool name")) {
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        return 1;
    }

    char* unescaped = NULL;
    size_t unescaped_len = 0;
    if (!require(json_unescape_alloc(result.tool_calls[0].arguments, result.tool_calls[0].arguments_len, &unescaped,
                                     &unescaped_len),
                 "unescape args")) {
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        return 1;
    }

    jstoktok_t* tokens = NULL;
    int count = 0;
    if (!require(parse_json_tokens(unescaped, unescaped_len, &tokens, &count), "parse args")) {
        free(unescaped);
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(tokens[0].type == JSTOK_OBJECT, "args object")) {
        free(tokens);
        free(unescaped);
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(json_expect_number(unescaped, tokens, count, 0, "a", 1), "args a")) {
        free(tokens);
        free(unescaped);
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(json_expect_string(unescaped, tokens, count, 0, "note", "hi\\nthere"), "args note")) {
        free(tokens);
        free(unescaped);
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        return 1;
    }

    free(tokens);
    free(unescaped);
    llm_chat_result_free(&result);

    fake_reset();
    const char* stream_payload =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_0\","
        "\"function\":{\"name\":\"ping\",\"arguments\":\"{\\\"a\\\":1,\\\"note\\\":\\\"hi\"}}]}}]}\n\n"
        "data: "
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\\nthere\\\"}\"}}]}}]}"
        "\n\n"
        "data: [DONE]\n\n";
    g_fake->stream_payload = stream_payload;
    g_fake->stream_payload_len = strlen(stream_payload);
    fake_stream_chunk_t chunks[3];
    size_t offset = 0;
    chunks[0].data = stream_payload;
    chunks[0].len = 5;
    offset += chunks[0].len;
    chunks[1].data = stream_payload + offset;
    chunks[1].len = 7;
    offset += chunks[1].len;
    if (!require(g_fake->stream_payload_len > offset, "stream payload too short")) {
        llm_client_destroy(client);
        return 1;
    }
    chunks[2].data = stream_payload + offset;
    chunks[2].len = g_fake->stream_payload_len - offset;
    g_fake->stream_chunks = chunks;
    g_fake->stream_chunks_count = 3;

    struct tool_delta_capture cap = {0};
    llm_stream_callbacks_t cbs = {0};
    cbs.user_data = &cap;
    cbs.on_tool_call_delta = on_tool_delta;
    cbs.on_tool_args_fragment = on_tool_args_fragment;
    cbs.on_tool_args_complete = on_tool_args_complete;

    if (!require(llm_chat_stream(client, messages, 1, NULL, NULL, NULL, &cbs), "stream chat")) {
        llm_client_destroy(client);
        return 1;
    }

    if (!require(g_fake->called_stream, "stream transport not called")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(!cap.overflow, "capture overflow")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(cap.calls == 2, "tool delta callbacks")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(cap.args_calls == 2, "tool args fragment callbacks")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(cap.indices[0] == 0 && cap.indices[1] == 0, "tool index")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(cap.id_len == 6 && memcmp(cap.id, "call_0", 6) == 0, "delta id")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(cap.name_len == 4 && memcmp(cap.name, "ping", 4) == 0, "delta name")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(cap.missing_id == 1 && cap.missing_name == 1, "missing id/name")) {
        llm_client_destroy(client);
        return 1;
    }

    const char* expected_raw = "{\\\"a\\\":1,\\\"note\\\":\\\"hi\\\\nthere\\\"}";
    if (!require(cap.args_len == strlen(expected_raw) && memcmp(cap.args, expected_raw, cap.args_len) == 0,
                 "raw args fragments")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(cap.args_frag_len == cap.args_len && memcmp(cap.args_frag, cap.args, cap.args_len) == 0,
                 "args fragment callbacks")) {
        llm_client_destroy(client);
        return 1;
    }

    unescaped = NULL;
    unescaped_len = 0;
    if (!require(json_unescape_alloc(cap.args, cap.args_len, &unescaped, &unescaped_len), "unescape stream args")) {
        llm_client_destroy(client);
        return 1;
    }
    tokens = NULL;
    count = 0;
    if (!require(parse_json_tokens(unescaped, unescaped_len, &tokens, &count), "parse stream args")) {
        free(unescaped);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(tokens[0].type == JSTOK_OBJECT, "stream args object")) {
        free(tokens);
        free(unescaped);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(json_expect_number(unescaped, tokens, count, 0, "a", 1), "stream args a")) {
        free(tokens);
        free(unescaped);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(json_expect_string(unescaped, tokens, count, 0, "note", "hi\\nthere"), "stream args note")) {
        free(tokens);
        free(unescaped);
        llm_client_destroy(client);
        return 1;
    }

    free(tokens);
    free(unescaped);
    tokens = NULL;
    count = 0;

    if (!require(cap.final_calls == 1, "final args callback count")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(cap.final_indices[0] == 0, "final args index")) {
        llm_client_destroy(client);
        return 1;
    }
    const char* expected_final = "{\"a\":1,\"note\":\"hi\\nthere\"}";
    if (!require(cap.final_args_len == strlen(expected_final) &&
                     memcmp(cap.final_args, expected_final, cap.final_args_len) == 0,
                 "final args json")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(parse_json_tokens(cap.final_args, cap.final_args_len, &tokens, &count), "parse final args")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(tokens[0].type == JSTOK_OBJECT, "final args object")) {
        free(tokens);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(json_expect_number(cap.final_args, tokens, count, 0, "a", 1), "final args a")) {
        free(tokens);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(json_expect_string(cap.final_args, tokens, count, 0, "note", "hi\\nthere"), "final args note")) {
        free(tokens);
        llm_client_destroy(client);
        return 1;
    }
    free(tokens);

    fake_reset();
    const char* multi_stream_payload =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
        "{\"index\":0,\"id\":\"call_0\",\"function\":{\"name\":\"ping\",\"arguments\":\"{\\\"a\\\":\"}},"
        "{\"index\":1,\"id\":\"call_1\",\"function\":{\"name\":\"pong\",\"arguments\":\"{\\\"b\\\":2\"}}"
        "]}}]}\n\n"
        "data: "
        "{\"choices\":[{\"delta\":{\"tool_calls\":["
        "{\"index\":0,\"function\":{\"arguments\":\"1}\"}},"
        "{\"index\":1,\"function\":{\"arguments\":\"}\"}}"
        "]}}]}\n\n"
        "data: [DONE]\n\n";
    g_fake->stream_payload = multi_stream_payload;
    g_fake->stream_payload_len = strlen(multi_stream_payload);
    g_fake->stream_chunk_size = 9;

    struct tool_multi_capture multi = {0};
    llm_stream_callbacks_t multi_cbs = {0};
    multi_cbs.user_data = &multi;
    multi_cbs.on_tool_call_delta = on_tool_delta_multi;
    multi_cbs.on_tool_args_complete = on_tool_args_complete_multi;

    if (!require(llm_chat_stream(client, messages, 1, NULL, NULL, NULL, &multi_cbs), "stream chat multi")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(g_fake->called_stream, "stream transport not called (multi)")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(!multi.overflow, "multi capture overflow")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(multi.delta_calls == 4, "multi tool delta callbacks")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(multi.fragment_calls[0] == 2 && multi.fragment_calls[1] == 2, "multi tool fragment counts")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(multi.id_len[0] == 6 && memcmp(multi.id[0], "call_0", 6) == 0, "multi tool 0 id")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(multi.name_len[0] == 4 && memcmp(multi.name[0], "ping", 4) == 0, "multi tool 0 name")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(multi.id_len[1] == 6 && memcmp(multi.id[1], "call_1", 6) == 0, "multi tool 1 id")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(multi.name_len[1] == 4 && memcmp(multi.name[1], "pong", 4) == 0, "multi tool 1 name")) {
        llm_client_destroy(client);
        return 1;
    }

    const char* expected_raw0 = "{\\\"a\\\":1}";
    const char* expected_raw1 = "{\\\"b\\\":2}";
    if (!require(multi.raw_args_len[0] == strlen(expected_raw0) &&
                     memcmp(multi.raw_args[0], expected_raw0, multi.raw_args_len[0]) == 0,
                 "multi raw args 0"))
        return 1;
    if (!require(multi.raw_args_len[1] == strlen(expected_raw1) &&
                     memcmp(multi.raw_args[1], expected_raw1, multi.raw_args_len[1]) == 0,
                 "multi raw args 1"))
        return 1;

    if (!require(multi.final_calls == 2, "multi final args callback count")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(multi.final_indices[0] == 0 && multi.final_indices[1] == 1, "multi final args indices")) {
        llm_client_destroy(client);
        return 1;
    }

    tokens = NULL;
    count = 0;
    if (!require(parse_json_tokens(multi.final_args[0], multi.final_args_len[0], &tokens, &count),
                 "parse multi final args 0")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(tokens[0].type == JSTOK_OBJECT, "multi final args 0 object")) {
        free(tokens);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(json_expect_number(multi.final_args[0], tokens, count, 0, "a", 1), "multi final args 0 a")) {
        free(tokens);
        llm_client_destroy(client);
        return 1;
    }
    free(tokens);

    tokens = NULL;
    count = 0;
    if (!require(parse_json_tokens(multi.final_args[1], multi.final_args_len[1], &tokens, &count),
                 "parse multi final args 1")) {
        llm_client_destroy(client);
        return 1;
    }
    if (!require(tokens[0].type == JSTOK_OBJECT, "multi final args 1 object")) {
        free(tokens);
        llm_client_destroy(client);
        return 1;
    }
    if (!require(json_expect_number(multi.final_args[1], tokens, count, 0, "b", 2), "multi final args 1 b")) {
        free(tokens);
        llm_client_destroy(client);
        return 1;
    }
    free(tokens);

    fake_reset();
    const char* bad_stream_payload =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_0\","
        "\"function\":{\"name\":\"ping\",\"arguments\":\"{\\\"a\\\":}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    g_fake->stream_payload = bad_stream_payload;
    g_fake->stream_payload_len = strlen(bad_stream_payload);
    g_fake->stream_chunk_size = 5;

    struct tool_delta_capture bad_cap = {0};
    llm_stream_callbacks_t bad_cbs = {0};
    bad_cbs.user_data = &bad_cap;
    bad_cbs.on_tool_call_delta = on_tool_delta;
    bad_cbs.on_tool_args_fragment = on_tool_args_fragment;
    bad_cbs.on_tool_args_complete = on_tool_args_complete;

    if (llm_chat_stream(client, messages, 1, NULL, NULL, NULL, &bad_cbs)) {
        fprintf(stderr, "Invalid tool args should fail\n");
        llm_client_destroy(client);
        return 1;
    }
    if (!require(bad_cap.final_calls == 0, "final args callback on invalid args")) {
        llm_client_destroy(client);
        return 1;
    }

    llm_client_destroy(client);

    printf("Tool delta callbacks test passed.\n");
    return 0;
}
