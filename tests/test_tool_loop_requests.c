#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/json_core.h"
#include "llm/llm.h"
#include "transport_curl.h"

static void assert_true(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
}

struct fake_transport {
    const char* post_responses[2];
    size_t post_calls;
    char* request_bodies[2];
    size_t request_lens[2];
};

static struct fake_transport g_fake;

static void fake_reset(void) {
    for (size_t i = 0; i < (sizeof(g_fake.request_bodies) / sizeof(g_fake.request_bodies[0])); i++) {
        free(g_fake.request_bodies[i]);
    }
    memset(&g_fake, 0, sizeof(g_fake));
}

bool http_get(const char* url, long timeout_ms, size_t max_response_bytes, const char* const* headers,
              size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url, const char* no_proxy,
              char** body, size_t* len) {
    (void)url;
    (void)timeout_ms;
    (void)max_response_bytes;
    (void)headers;
    (void)headers_count;
    (void)tls;
    (void)proxy_url;
    (void)no_proxy;
    if (body) *body = NULL;
    if (len) *len = 0;
    return false;
}

bool http_post(const char* url, const char* json_body, long timeout_ms, size_t max_response_bytes,
               const char* const* headers, size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url,
               const char* no_proxy, char** body, size_t* len) {
    (void)url;
    (void)timeout_ms;
    (void)max_response_bytes;
    (void)headers;
    (void)headers_count;
    (void)tls;
    (void)proxy_url;
    (void)no_proxy;

    if (g_fake.post_calls >= (sizeof(g_fake.post_responses) / sizeof(g_fake.post_responses[0]))) {
        if (body) *body = NULL;
        if (len) *len = 0;
        return false;
    }

    if (json_body) {
        size_t req_len = strlen(json_body);
        char* req = malloc(req_len + 1);
        if (!req) return false;
        memcpy(req, json_body, req_len);
        req[req_len] = '\0';
        g_fake.request_bodies[g_fake.post_calls] = req;
        g_fake.request_lens[g_fake.post_calls] = req_len;
    }

    const char* resp = g_fake.post_responses[g_fake.post_calls++];
    if (!resp) {
        if (body) *body = NULL;
        if (len) *len = 0;
        return false;
    }
    size_t resp_len = strlen(resp);
    char* out = malloc(resp_len + 1);
    if (!out) return false;
    memcpy(out, resp, resp_len);
    out[resp_len] = '\0';
    if (body) *body = out;
    if (len) *len = resp_len;
    return true;
}

bool http_post_stream(const char* url, const char* json_body, long timeout_ms, long read_idle_timeout_ms,
                      const char* const* headers, size_t headers_count, const llm_tls_config_t* tls,
                      const char* proxy_url, const char* no_proxy, stream_cb cb, void* user_data) {
    (void)url;
    (void)json_body;
    (void)timeout_ms;
    (void)read_idle_timeout_ms;
    (void)headers;
    (void)headers_count;
    (void)tls;
    (void)proxy_url;
    (void)no_proxy;
    (void)cb;
    (void)user_data;
    return false;
}

static bool extract_assistant_tool_call_fields(const char* json, size_t len, span_t* id, span_t* name, span_t* args) {
    bool ok = false;
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return false;
    jstoktok_t* tokens = malloc((size_t)needed * sizeof(*tokens));
    if (!tokens) return false;
    jstok_init(&parser);
    int parsed = jstok_parse(&parser, json, (int)len, tokens, needed);
    if (parsed <= 0 || tokens[0].type != JSTOK_OBJECT) goto cleanup;
    int messages_idx = obj_get_key(tokens, parsed, 0, json, "messages");
    if (messages_idx < 0 || tokens[messages_idx].type != JSTOK_ARRAY) goto cleanup;
    for (int i = 0; i < tokens[messages_idx].size; i++) {
        int msg_idx = arr_get(tokens, parsed, messages_idx, i);
        if (msg_idx < 0 || tokens[msg_idx].type != JSTOK_OBJECT) continue;
        int role_idx = obj_get_key(tokens, parsed, msg_idx, json, "role");
        if (role_idx < 0 || tokens[role_idx].type != JSTOK_STRING) continue;
        span_t role = tok_span(json, &tokens[role_idx]);
        if (role.len != 9 || memcmp(role.ptr, "assistant", 9) != 0) continue;
        int tool_calls_idx = obj_get_key(tokens, parsed, msg_idx, json, "tool_calls");
        if (tool_calls_idx < 0 || tokens[tool_calls_idx].type != JSTOK_ARRAY || tokens[tool_calls_idx].size <= 0) {
            goto cleanup;
        }
        int tool_idx = arr_get(tokens, parsed, tool_calls_idx, 0);
        if (tool_idx < 0 || tokens[tool_idx].type != JSTOK_OBJECT) goto cleanup;
        int id_idx = obj_get_key(tokens, parsed, tool_idx, json, "id");
        int func_idx = obj_get_key(tokens, parsed, tool_idx, json, "function");
        if (id_idx < 0 || tokens[id_idx].type != JSTOK_STRING) goto cleanup;
        if (func_idx < 0 || tokens[func_idx].type != JSTOK_OBJECT) goto cleanup;
        int name_idx = obj_get_key(tokens, parsed, func_idx, json, "name");
        int args_idx = obj_get_key(tokens, parsed, func_idx, json, "arguments");
        if (name_idx < 0 || tokens[name_idx].type != JSTOK_STRING) goto cleanup;
        if (args_idx < 0 || tokens[args_idx].type != JSTOK_STRING) goto cleanup;
        *id = tok_span(json, &tokens[id_idx]);
        *name = tok_span(json, &tokens[name_idx]);
        *args = tok_span(json, &tokens[args_idx]);
        ok = true;
        break;
    }

cleanup:
    free(tokens);
    return ok;
}

static bool tool_dispatch(void* user_data, const char* tool_name, size_t name_len, const char* args_json,
                          size_t args_len, char** result_json, size_t* result_len) {
    (void)user_data;
    (void)args_json;
    (void)args_len;
    if (name_len == 3 && memcmp(tool_name, "add", 3) == 0) {
        *result_json = strdup("43");
        if (!*result_json) return false;
        *result_len = 2;
        return true;
    }
    return false;
}

static bool test_tool_loop_includes_tool_calls(void) {
    fake_reset();
    g_fake.post_responses[0] =
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
        "\"name\":\"add\",\"arguments\":\"42\"}}]},\"finish_reason\":\"tool_calls\"}]}";
    g_fake.post_responses[1] = "{\"choices\":[{\"message\":{\"content\":\"done\"},\"finish_reason\":\"stop\"}]}";

    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {1000, 2000, 2000};
    llm_limits_t limits = {0};
    limits.max_response_bytes = 64 * 1024;
    limits.max_line_bytes = 1024;
    limits.max_frame_bytes = 1024;
    limits.max_sse_buffer_bytes = 64 * 1024;
    limits.max_tool_args_bytes_per_call = 1024;
    llm_client_t* client = llm_client_create("http://fake", &model, &timeout, &limits);
    assert_true(client != NULL, "client create failed");

    const char* tooling_json = "{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"add\"}}]}";
    llm_message_t msg = {LLM_ROLE_USER, "run tool", 8, NULL, 0, NULL, 0, NULL, 0};

    bool ok = llm_tool_loop_run(client, &msg, 1, tooling_json, tool_dispatch, NULL, 3);
    assert_true(ok, "tool loop failed");
    assert_true(g_fake.post_calls == 2, "expected two post calls");

    span_t id = {0};
    span_t name = {0};
    span_t args = {0};
    assert_true(extract_assistant_tool_call_fields(g_fake.request_bodies[1], g_fake.request_lens[1], &id, &name, &args),
                "assistant tool_calls missing from follow-up request");
    assert_true(id.len == 6 && memcmp(id.ptr, "call_1", 6) == 0, "tool id mismatch");
    assert_true(name.len == 3 && memcmp(name.ptr, "add", 3) == 0, "tool name mismatch");
    assert_true(args.len == 2 && memcmp(args.ptr, "42", 2) == 0, "tool args mismatch");

    llm_client_destroy(client);
    return true;
}

int main(void) {
    assert_true(test_tool_loop_includes_tool_calls(), "tool loop tool_calls request test failed");
    printf("Tool loop request tests passed.\n");
    return 0;
}
