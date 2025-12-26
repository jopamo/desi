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
    const char* stream_payload;
    size_t stream_payload_len;
    size_t stream_chunk_size;
    size_t stream_cb_calls;
    const char* post_responses[2];
    size_t post_calls;
};

static struct fake_transport g_fake;

static void fake_reset(void) { memset(&g_fake, 0, sizeof(g_fake)); }

bool http_get(const char* url, long timeout_ms, size_t max_response_bytes, const char* const* headers,
              size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url, const char* no_proxy,
              char** body, size_t* len, llm_transport_status_t* status) {
    (void)url;
    (void)timeout_ms;
    (void)max_response_bytes;
    (void)headers;
    (void)headers_count;
    (void)tls;
    (void)proxy_url;
    (void)no_proxy;
    if (status) {
        status->http_status = 0;
        status->curl_code = 0;
        status->tls_error = false;
    }
    if (body) *body = NULL;
    if (len) *len = 0;
    return false;
}

bool http_post(const char* url, const char* json_body, long timeout_ms, size_t max_response_bytes,
               const char* const* headers, size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url,
               const char* no_proxy, char** body, size_t* len, llm_transport_status_t* status) {
    (void)url;
    (void)json_body;
    (void)timeout_ms;
    (void)max_response_bytes;
    (void)headers;
    (void)headers_count;
    (void)tls;
    (void)proxy_url;
    (void)no_proxy;
    if (status) {
        status->http_status = 200;
        status->curl_code = 0;
        status->tls_error = false;
    }
    if (g_fake.post_calls >= (sizeof(g_fake.post_responses) / sizeof(g_fake.post_responses[0]))) {
        if (body) *body = NULL;
        if (len) *len = 0;
        if (status) status->http_status = 0;
        return false;
    }
    const char* resp = g_fake.post_responses[g_fake.post_calls++];
    if (!resp) {
        if (body) *body = NULL;
        if (len) *len = 0;
        if (status) status->http_status = 0;
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
                      const char* proxy_url, const char* no_proxy, stream_cb cb, void* user_data,
                      llm_transport_status_t* status) {
    (void)url;
    (void)json_body;
    (void)timeout_ms;
    (void)read_idle_timeout_ms;
    (void)headers;
    (void)headers_count;
    (void)tls;
    (void)proxy_url;
    (void)no_proxy;
    if (status) {
        status->http_status = 200;
        status->curl_code = 0;
        status->tls_error = false;
    }
    if (!g_fake.stream_payload || g_fake.stream_payload_len == 0) return true;
    size_t chunk_size = g_fake.stream_chunk_size ? g_fake.stream_chunk_size : g_fake.stream_payload_len;
    size_t offset = 0;
    while (offset < g_fake.stream_payload_len) {
        size_t remaining = g_fake.stream_payload_len - offset;
        size_t take = remaining < chunk_size ? remaining : chunk_size;
        if (!cb(g_fake.stream_payload + offset, take, user_data)) return false;
        g_fake.stream_cb_calls++;
        offset += take;
    }
    return true;
}

static bool extract_chat_delta_content(const char* json, size_t len, span_t* out) {
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
    int choices_idx = obj_get_key(tokens, parsed, 0, json, "choices");
    if (choices_idx < 0 || tokens[choices_idx].type != JSTOK_ARRAY || tokens[choices_idx].size <= 0) goto cleanup;
    int choice_idx = arr_get(tokens, parsed, choices_idx, 0);
    if (choice_idx < 0 || tokens[choice_idx].type != JSTOK_OBJECT) goto cleanup;
    int delta_idx = obj_get_key(tokens, parsed, choice_idx, json, "delta");
    if (delta_idx < 0 || tokens[delta_idx].type != JSTOK_OBJECT) goto cleanup;
    int content_idx = obj_get_key(tokens, parsed, delta_idx, json, "content");
    if (content_idx < 0 || tokens[content_idx].type != JSTOK_STRING) goto cleanup;
    *out = tok_span(json, &tokens[content_idx]);
    ok = true;

cleanup:
    free(tokens);
    return ok;
}

static bool extract_tool_args(const char* json, size_t len, span_t* out) {
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
    int choices_idx = obj_get_key(tokens, parsed, 0, json, "choices");
    if (choices_idx < 0 || tokens[choices_idx].type != JSTOK_ARRAY || tokens[choices_idx].size <= 0) goto cleanup;
    int choice_idx = arr_get(tokens, parsed, choices_idx, 0);
    if (choice_idx < 0 || tokens[choice_idx].type != JSTOK_OBJECT) goto cleanup;
    int message_idx = obj_get_key(tokens, parsed, choice_idx, json, "message");
    if (message_idx < 0 || tokens[message_idx].type != JSTOK_OBJECT) goto cleanup;
    int tool_calls_idx = obj_get_key(tokens, parsed, message_idx, json, "tool_calls");
    if (tool_calls_idx < 0 || tokens[tool_calls_idx].type != JSTOK_ARRAY || tokens[tool_calls_idx].size <= 0) {
        goto cleanup;
    }
    int tool_idx = arr_get(tokens, parsed, tool_calls_idx, 0);
    if (tool_idx < 0 || tokens[tool_idx].type != JSTOK_OBJECT) goto cleanup;
    int func_idx = obj_get_key(tokens, parsed, tool_idx, json, "function");
    if (func_idx < 0 || tokens[func_idx].type != JSTOK_OBJECT) goto cleanup;
    int args_idx = obj_get_key(tokens, parsed, func_idx, json, "arguments");
    if (args_idx < 0 || tokens[args_idx].type != JSTOK_STRING) goto cleanup;
    *out = tok_span(json, &tokens[args_idx]);
    ok = true;

cleanup:
    free(tokens);
    return ok;
}

struct stream_capture {
    char content[64];
    size_t len;
    size_t frames;
};

static void on_content_delta(void* user_data, const char* delta, size_t len) {
    struct stream_capture* cap = user_data;
    if (cap->len + len < sizeof(cap->content)) {
        memcpy(cap->content + cap->len, delta, len);
        cap->len += len;
        cap->content[cap->len] = '\0';
    }
    cap->frames++;
}

static bool abort_after_first_frame(void* user_data) {
    struct stream_capture* cap = user_data;
    return cap->frames > 0;
}

static bool test_stream_cancel_after_frame(void) {
    fake_reset();

    static const char frame1[] = "{\"choices\":[{\"delta\":{\"content\":\"hello\"},\"finish_reason\":null}]}";
    static const char frame2[] = "{\"choices\":[{\"delta\":{\"content\":\"world\"},\"finish_reason\":null}]}";
    static char stream_buf[512];
    int n = snprintf(stream_buf, sizeof(stream_buf),
                     "data: %s\n\n"
                     "data: %s\n\n"
                     "data: [DONE]\n\n",
                     frame1, frame2);
    assert_true(n > 0 && (size_t)n < sizeof(stream_buf), "stream buffer overflow");

    g_fake.stream_payload = stream_buf;
    g_fake.stream_payload_len = (size_t)n;
    g_fake.stream_chunk_size = g_fake.stream_payload_len;

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

    llm_message_t msg = {LLM_ROLE_USER, "ping", 4, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    struct stream_capture cap = {0};
    llm_stream_callbacks_t callbacks = {0};
    callbacks.user_data = &cap;
    callbacks.on_content_delta = on_content_delta;

    llm_error_t err = llm_chat_stream_ex(client, &msg, 1, NULL, NULL, NULL, &callbacks, abort_after_first_frame, &cap);
    assert_true(err == LLM_ERR_CANCELLED, "expected cancellation after frame");

    span_t expected = {0};
    assert_true(extract_chat_delta_content(frame1, strlen(frame1), &expected), "parse frame1 content failed");
    assert_true(cap.len == expected.len, "content length mismatch");
    assert_true(memcmp(cap.content, expected.ptr, expected.len) == 0, "content mismatch after cancel");

    llm_client_destroy(client);
    return true;
}

struct abort_once {
    size_t calls;
};

static bool abort_on_first_call(void* user_data) {
    struct abort_once* state = user_data;
    state->calls++;
    return state->calls >= 1;
}

static bool test_stream_cancel_after_chunk(void) {
    fake_reset();

    static const char stream_sse[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":null}]}\n\n"
        "data: [DONE]\n\n";
    g_fake.stream_payload = stream_sse;
    g_fake.stream_payload_len = strlen(stream_sse);
    g_fake.stream_chunk_size = 5;

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

    llm_message_t msg = {LLM_ROLE_USER, "ping", 4, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    struct stream_capture cap = {0};
    llm_stream_callbacks_t callbacks = {0};
    callbacks.user_data = &cap;
    callbacks.on_content_delta = on_content_delta;
    struct abort_once abort_state = {0};

    llm_error_t err =
        llm_chat_stream_ex(client, &msg, 1, NULL, NULL, NULL, &callbacks, abort_on_first_call, &abort_state);
    assert_true(err == LLM_ERR_CANCELLED, "expected cancellation after chunk");
    assert_true(cap.len == 0, "content should be empty on early cancel");

    llm_client_destroy(client);
    return true;
}

struct tool_state {
    size_t called;
    bool args_ok;
    bool cancel;
};

static bool tool_dispatch(void* user_data, const char* tool_name, size_t name_len, const char* args_json,
                          size_t args_len, char** result_json, size_t* result_len) {
    struct tool_state* state = user_data;
    state->called++;
    state->cancel = true;
    state->args_ok = (name_len == 3 && memcmp(tool_name, "add", 3) == 0);
    if (state->args_ok) {
        span_t expected = {0};
        const char* resp = g_fake.post_responses[0];
        if (!resp) {
            state->args_ok = false;
        } else if (!extract_tool_args(resp, strlen(resp), &expected)) {
            state->args_ok = false;
        } else {
            state->args_ok = (args_len == expected.len && memcmp(args_json, expected.ptr, expected.len) == 0);
        }
    }
    *result_json = strdup("43");
    if (!*result_json) return false;
    *result_len = 2;
    return true;
}

static bool abort_tool_loop(void* user_data) {
    struct tool_state* state = user_data;
    return state->cancel;
}

static bool test_tool_loop_cancel_between_turns(void) {
    fake_reset();
    g_fake.post_responses[0] =
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_1\",\"function\":{\"name\":\"add\",\"arguments\":"
        "\"42\"}}]},\"finish_reason\":\"tool_calls\"}]}";
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
    llm_message_t msg = {LLM_ROLE_SYSTEM, "run tool", 8, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    struct tool_state state = {0};

    llm_error_t err = llm_tool_loop_run_ex(client, &msg, 1, NULL, tooling_json, NULL, tool_dispatch, &state,
                                           abort_tool_loop, &state, 3);
    assert_true(err == LLM_ERR_CANCELLED, "expected cancellation between turns");
    assert_true(state.called == 1, "expected one tool dispatch before cancel");
    assert_true(state.args_ok, "tool args parse failed");
    assert_true(g_fake.post_calls == 1, "expected one post call before cancel");

    llm_client_destroy(client);
    return true;
}

int main(void) {
    assert_true(test_stream_cancel_after_frame(), "stream cancel after frame failed");
    assert_true(test_stream_cancel_after_chunk(), "stream cancel after chunk failed");
    assert_true(test_tool_loop_cancel_between_turns(), "tool loop cancel failed");
    printf("Cancellation tests passed.\n");
    return 0;
}
