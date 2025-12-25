#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/json_core.h"
#include "llm/llm.h"
#include "transport_curl.h"

#define ASSERT(cond, msg)                       \
    do {                                        \
        if (!(cond)) {                          \
            fprintf(stderr, "FAIL: %s\n", msg); \
            return false;                       \
        }                                       \
    } while (0)

struct fake_transport_state {
    const char* expected_url;
    const char* const* expected_headers;
    size_t expected_headers_count;
    const char* response_get;
    const char* response_post;
    char* last_body;
    size_t last_body_len;
    const char* stream_payload;
    size_t stream_payload_len;
    size_t stream_chunk_size;
    bool fail_get;
    bool fail_post;
    bool fail_stream;
    bool headers_ok;
    size_t stream_cb_calls;
};

static struct fake_transport_state g_fake;

static void fake_reset(void) { memset(&g_fake, 0, sizeof(g_fake)); }

static bool header_list_contains(const char* const* headers, size_t headers_count, const char* expected) {
    for (size_t i = 0; i < headers_count; i++) {
        if (headers[i] && strcmp(headers[i], expected) == 0) return true;
    }
    return false;
}

static bool check_headers(const char* const* headers, size_t headers_count) {
    if (g_fake.expected_headers_count == 0) return true;
    if (!headers || headers_count < g_fake.expected_headers_count) return false;
    for (size_t i = 0; i < g_fake.expected_headers_count; i++) {
        if (!header_list_contains(headers, headers_count, g_fake.expected_headers[i])) return false;
    }
    return true;
}

static bool extract_choice_content(const char* json, size_t len, const char* obj_key, span_t* out) {
    bool ok = false;
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return false;
    jstoktok_t* tokens = malloc((size_t)needed * sizeof(*tokens));
    if (!tokens) return false;
    jstok_init(&parser);
    int parsed = jstok_parse(&parser, json, (int)len, tokens, needed);
    if (parsed <= 0) goto cleanup;
    if (tokens[0].type != JSTOK_OBJECT) goto cleanup;
    int choices_idx = obj_get_key(tokens, parsed, 0, json, "choices");
    if (choices_idx < 0 || tokens[choices_idx].type != JSTOK_ARRAY || tokens[choices_idx].size <= 0) goto cleanup;
    int choice_idx = arr_get(tokens, parsed, choices_idx, 0);
    if (choice_idx < 0 || tokens[choice_idx].type != JSTOK_OBJECT) goto cleanup;
    int obj_idx = obj_get_key(tokens, parsed, choice_idx, json, obj_key);
    if (obj_idx < 0 || tokens[obj_idx].type != JSTOK_OBJECT) goto cleanup;
    int content_idx = obj_get_key(tokens, parsed, obj_idx, json, "content");
    if (content_idx < 0 || tokens[content_idx].type != JSTOK_STRING) goto cleanup;
    *out = tok_span(json, &tokens[content_idx]);
    ok = true;

cleanup:
    free(tokens);
    return ok;
}

bool http_get(const char* url, long timeout_ms, size_t max_response_bytes, const char* const* headers,
              size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url, char** body, size_t* len) {
    (void)timeout_ms;
    (void)max_response_bytes;
    (void)tls;
    (void)proxy_url;

    g_fake.headers_ok = g_fake.headers_ok && check_headers(headers, headers_count);
    if (g_fake.expected_url && strcmp(url, g_fake.expected_url) != 0) {
        g_fake.headers_ok = false;
    }
    if (g_fake.fail_get) {
        if (body) *body = NULL;
        if (len) *len = 0;
        return false;
    }
    if (!g_fake.response_get) {
        if (body) *body = NULL;
        if (len) *len = 0;
        return false;
    }
    size_t resp_len = strlen(g_fake.response_get);
    char* resp = malloc(resp_len + 1);
    if (!resp) return false;
    memcpy(resp, g_fake.response_get, resp_len);
    resp[resp_len] = '\0';
    g_fake.last_body = resp;
    g_fake.last_body_len = resp_len;
    if (body) *body = resp;
    if (len) *len = resp_len;
    return true;
}

bool http_post(const char* url, const char* json_body, long timeout_ms, size_t max_response_bytes,
               const char* const* headers, size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url,
               char** body, size_t* len) {
    (void)json_body;
    (void)timeout_ms;
    (void)max_response_bytes;
    (void)tls;
    (void)proxy_url;

    g_fake.headers_ok = g_fake.headers_ok && check_headers(headers, headers_count);
    if (g_fake.expected_url && strcmp(url, g_fake.expected_url) != 0) {
        g_fake.headers_ok = false;
    }
    if (g_fake.fail_post) {
        if (body) *body = NULL;
        if (len) *len = 0;
        return false;
    }
    if (!g_fake.response_post) {
        if (body) *body = NULL;
        if (len) *len = 0;
        return false;
    }
    size_t resp_len = strlen(g_fake.response_post);
    char* resp = malloc(resp_len + 1);
    if (!resp) return false;
    memcpy(resp, g_fake.response_post, resp_len);
    resp[resp_len] = '\0';
    g_fake.last_body = resp;
    g_fake.last_body_len = resp_len;
    if (body) *body = resp;
    if (len) *len = resp_len;
    return true;
}

bool http_post_stream(const char* url, const char* json_body, long timeout_ms, long read_idle_timeout_ms,
                      const char* const* headers, size_t headers_count, const llm_tls_config_t* tls,
                      const char* proxy_url, stream_cb cb, void* user_data) {
    (void)json_body;
    (void)timeout_ms;
    (void)read_idle_timeout_ms;
    (void)tls;
    (void)proxy_url;

    g_fake.headers_ok = g_fake.headers_ok && check_headers(headers, headers_count);
    if (g_fake.expected_url && strcmp(url, g_fake.expected_url) != 0) {
        g_fake.headers_ok = false;
    }
    if (g_fake.fail_stream) return false;
    if (!g_fake.stream_payload || g_fake.stream_payload_len == 0) return true;

    const size_t chunk_size = g_fake.stream_chunk_size ? g_fake.stream_chunk_size : g_fake.stream_payload_len;
    if (chunk_size > 128) return false;

    char chunk[128];
    size_t offset = 0;
    while (offset < g_fake.stream_payload_len) {
        size_t remaining = g_fake.stream_payload_len - offset;
        size_t take = remaining < chunk_size ? remaining : chunk_size;
        memcpy(chunk, g_fake.stream_payload + offset, take);
        g_fake.headers_ok = g_fake.headers_ok && check_headers(headers, headers_count);
        cb(chunk, take, user_data);
        g_fake.stream_cb_calls++;
        g_fake.headers_ok = g_fake.headers_ok && check_headers(headers, headers_count);
        memset(chunk, 'x', take);
        offset += take;
    }
    return true;
}

struct stream_capture {
    char content[64];
    size_t len;
    llm_finish_reason_t finish_reason;
};

static void on_content_delta(void* user_data, const char* delta, size_t len) {
    struct stream_capture* cap = user_data;
    if (cap->len + len >= sizeof(cap->content)) return;
    memcpy(cap->content + cap->len, delta, len);
    cap->len += len;
    cap->content[cap->len] = '\0';
}

static void on_finish_reason(void* user_data, llm_finish_reason_t reason) {
    struct stream_capture* cap = user_data;
    cap->finish_reason = reason;
}

static llm_client_t* make_client(const char* base_url, const char* const* headers, size_t headers_count) {
    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {0};
    timeout.connect_timeout_ms = 1000;
    timeout.overall_timeout_ms = 2000;
    timeout.read_idle_timeout_ms = 2000;
    llm_limits_t limits = {0};
    limits.max_response_bytes = 64 * 1024;
    limits.max_line_bytes = 1024;
    limits.max_tool_args_bytes_per_call = 1024;
    if (headers && headers_count > 0) {
        return llm_client_create_with_headers(base_url, &model, &timeout, &limits, headers, headers_count);
    }
    return llm_client_create(base_url, &model, &timeout, &limits);
}

static bool test_contract_body_ownership(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/chat/completions";
    g_fake.response_post =
        "{\"choices\":[{\"message\":{\"content\":\"hello\"},\"finish_reason\":\"stop\"}]}";

    llm_client_t* client = make_client("http://fake", NULL, 0);
    ASSERT(client, "client create failed");

    llm_message_t msg = {0};
    msg.role = LLM_ROLE_USER;
    msg.content = "hi";
    msg.content_len = 2;

    llm_chat_result_t result;
    bool ok = llm_chat(client, &msg, 1, NULL, NULL, NULL, &result);
    ASSERT(ok, "llm_chat failed");
    ASSERT(result._internal == g_fake.last_body, "response buffer not transferred");
    ASSERT(result.content, "missing content span");
    ASSERT(result.finish_reason == LLM_FINISH_REASON_STOP, "finish reason mismatch");

    span_t expected = {0};
    ASSERT(extract_choice_content(g_fake.last_body, g_fake.last_body_len, "message", &expected),
           "content parse failed");
    ASSERT(result.content_len == expected.len, "content length mismatch");
    ASSERT(memcmp(result.content, expected.ptr, expected.len) == 0, "content mismatch");

    llm_chat_result_free(&result);
    llm_client_destroy(client);
    ASSERT(g_fake.headers_ok, "headers unstable during non-stream request");
    return true;
}

static bool test_contract_streaming_headers(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/chat/completions";

    static const char stream_json[] =
        "{\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}";
    static const char stream_sse[] = "data: " "{\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}"
                                     "\n\n"
                                     "data: [DONE]\n\n";
    g_fake.stream_payload = stream_sse;
    g_fake.stream_payload_len = strlen(stream_sse);
    g_fake.stream_chunk_size = 16;

    const char* client_headers[] = {"X-Client: alpha"};
    llm_client_t* client = make_client("http://fake", client_headers, 1);
    ASSERT(client, "client create failed");
    ASSERT(llm_client_set_api_key(client, "token"), "set api key failed");

    const char* request_headers[] = {"X-Request: beta"};
    const char* expected_headers[] = {"X-Client: alpha", "Authorization: Bearer token", "X-Request: beta"};
    g_fake.expected_headers = expected_headers;
    g_fake.expected_headers_count = 3;

    llm_message_t msg = {0};
    msg.role = LLM_ROLE_USER;
    msg.content = "ping";
    msg.content_len = 4;

    struct stream_capture cap = {0};
    cap.finish_reason = LLM_FINISH_REASON_UNKNOWN;

    llm_stream_callbacks_t callbacks = {0};
    callbacks.user_data = &cap;
    callbacks.on_content_delta = on_content_delta;
    callbacks.on_finish_reason = on_finish_reason;

    bool ok = llm_chat_stream_with_headers(client, &msg, 1, NULL, NULL, NULL, &callbacks, request_headers, 1);
    ASSERT(ok, "llm_chat_stream failed");
    ASSERT(g_fake.stream_cb_calls > 0, "stream callback not invoked");
    ASSERT(g_fake.headers_ok, "headers unstable during stream callbacks");
    ASSERT(cap.finish_reason == LLM_FINISH_REASON_STOP, "finish reason mismatch");

    span_t expected = {0};
    ASSERT(extract_choice_content(stream_json, strlen(stream_json), "delta", &expected), "stream content parse failed");
    ASSERT(cap.len == expected.len, "stream content length mismatch");
    ASSERT(memcmp(cap.content, expected.ptr, expected.len) == 0, "stream content mismatch");

    llm_client_destroy(client);
    return true;
}

static bool test_contract_failure_propagation(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/health";
    g_fake.fail_get = true;

    llm_client_t* client = make_client("http://fake", NULL, 0);
    ASSERT(client, "client create failed");

    const char* json = "stale";
    size_t len = 123;
    bool ok = llm_props_get(client, &json, &len);
    ASSERT(!ok, "llm_props_get should fail");
    ASSERT(json == NULL, "failure should clear body");
    ASSERT(len == 0, "failure should clear length");

    g_fake.expected_url = "http://fake/v1/chat/completions";
    g_fake.fail_stream = true;
    llm_message_t msg = {0};
    msg.role = LLM_ROLE_USER;
    msg.content = "ping";
    msg.content_len = 4;
    llm_stream_callbacks_t callbacks = {0};
    ok = llm_chat_stream(client, &msg, 1, NULL, NULL, NULL, &callbacks);
    ASSERT(!ok, "llm_chat_stream should fail");
    ASSERT(g_fake.stream_cb_calls == 0, "stream callback should not run on failure");

    llm_client_destroy(client);
    ASSERT(g_fake.headers_ok, "headers unstable during failure tests");
    return true;
}

int main(void) {
    if (!test_contract_body_ownership()) return 1;
    if (!test_contract_streaming_headers()) return 1;
    if (!test_contract_failure_propagation()) return 1;
    printf("Transport contract tests passed.\n");
    return 0;
}
