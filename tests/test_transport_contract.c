#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/json_core.h"
#include "llm/llm.h"
#include "transport_curl.h"

static bool require(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return false;
    }
    return true;
}

struct fake_transport_state {
    const char* expected_url;
    const char* const* expected_headers;
    size_t expected_headers_count;
    const char* expected_proxy_url;
    const char* expected_no_proxy;
    long status_get;
    long status_post;
    long status_stream;
    const char* response_get;
    const char* response_post;
    char* last_body;
    size_t last_body_len;
    char* last_request_body;
    size_t last_request_len;
    const char* stream_payload;
    size_t stream_payload_len;
    size_t stream_chunk_size;
    bool fail_get;
    bool fail_post;
    bool fail_stream;
    bool called_get;
    bool called_post;
    bool called_stream;
    bool headers_ok;
    bool proxy_ok;
    size_t stream_cb_calls;
};

static struct fake_transport_state g_fake;

static void fake_reset(void) {
    free(g_fake.last_request_body);
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.status_get = 200;
    g_fake.status_post = 200;
    g_fake.status_stream = 200;
}

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

static bool check_proxy(const char* proxy_url, const char* no_proxy) {
    if (g_fake.expected_proxy_url) {
        if (!proxy_url || strcmp(proxy_url, g_fake.expected_proxy_url) != 0) return false;
    }
    if (g_fake.expected_no_proxy) {
        if (!no_proxy || strcmp(no_proxy, g_fake.expected_no_proxy) != 0) return false;
    }
    return true;
}

static bool extract_choice_content_at(const char* json, size_t len, size_t choice_index, const char* obj_key,
                                      span_t* out) {
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
    if (choice_index >= (size_t)tokens[choices_idx].size) goto cleanup;
    int choice_idx = arr_get(tokens, parsed, choices_idx, (int)choice_index);
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

static bool extract_choice_content(const char* json, size_t len, const char* obj_key, span_t* out) {
    return extract_choice_content_at(json, len, 0, obj_key, out);
}

static bool extract_completion_text_at(const char* json, size_t len, size_t choice_index, span_t* out) {
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
    if (choice_index >= (size_t)tokens[choices_idx].size) goto cleanup;
    int choice_idx = arr_get(tokens, parsed, choices_idx, (int)choice_index);
    if (choice_idx < 0 || tokens[choice_idx].type != JSTOK_OBJECT) goto cleanup;
    int text_idx = obj_get_key(tokens, parsed, choice_idx, json, "text");
    if (text_idx < 0 || tokens[text_idx].type != JSTOK_STRING) goto cleanup;
    *out = tok_span(json, &tokens[text_idx]);
    ok = true;

cleanup:
    free(tokens);
    return ok;
}

static bool extract_string_field(const char* json, size_t len, const char* key, span_t* out) {
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
    int key_idx = obj_get_key(tokens, parsed, 0, json, key);
    if (key_idx < 0 || tokens[key_idx].type != JSTOK_STRING) goto cleanup;
    *out = tok_span(json, &tokens[key_idx]);
    ok = true;

cleanup:
    free(tokens);
    return ok;
}

static bool extract_embedding_input_at(const char* json, size_t len, size_t input_index, span_t* out) {
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
    int input_idx = obj_get_key(tokens, parsed, 0, json, "input");
    if (input_idx < 0 || tokens[input_idx].type != JSTOK_ARRAY) goto cleanup;
    if (input_index >= (size_t)tokens[input_idx].size) goto cleanup;
    int item_idx = arr_get(tokens, parsed, input_idx, (int)input_index);
    if (item_idx < 0 || tokens[item_idx].type != JSTOK_STRING) goto cleanup;
    *out = tok_span(json, &tokens[item_idx]);
    ok = true;

cleanup:
    free(tokens);
    return ok;
}

static bool extract_embedding_span_at(const char* json, size_t len, size_t index, span_t* out) {
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
    int data_idx = obj_get_key(tokens, parsed, 0, json, "data");
    if (data_idx < 0 || tokens[data_idx].type != JSTOK_ARRAY || tokens[data_idx].size <= 0) goto cleanup;
    if (index >= (size_t)tokens[data_idx].size) goto cleanup;
    int item_idx = arr_get(tokens, parsed, data_idx, (int)index);
    if (item_idx < 0 || tokens[item_idx].type != JSTOK_OBJECT) goto cleanup;
    int emb_idx = obj_get_key(tokens, parsed, item_idx, json, "embedding");
    if (emb_idx < 0 || tokens[emb_idx].type != JSTOK_ARRAY) goto cleanup;
    *out = tok_span(json, &tokens[emb_idx]);
    ok = true;

cleanup:
    free(tokens);
    return ok;
}

bool http_get(const char* url, long timeout_ms, size_t max_response_bytes, const char* const* headers,
              size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url, const char* no_proxy,
              char** body, size_t* len, llm_transport_status_t* status) {
    (void)timeout_ms;
    (void)max_response_bytes;
    (void)tls;

    if (status) {
        status->http_status = g_fake.status_get;
        status->curl_code = 0;
        status->tls_error = false;
    }
    g_fake.called_get = true;
    g_fake.headers_ok = g_fake.headers_ok && check_headers(headers, headers_count);
    g_fake.proxy_ok = g_fake.proxy_ok && check_proxy(proxy_url, no_proxy);
    if (g_fake.expected_url && strcmp(url, g_fake.expected_url) != 0) {
        g_fake.headers_ok = false;
    }
    if (g_fake.fail_get) {
        if (body) *body = NULL;
        if (len) *len = 0;
        if (status) status->http_status = 0;
        return false;
    }
    if (!g_fake.response_get) {
        if (body) *body = NULL;
        if (len) *len = 0;
        if (status) status->http_status = 0;
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
               const char* no_proxy, char** body, size_t* len, llm_transport_status_t* status) {
    (void)timeout_ms;
    (void)max_response_bytes;
    (void)tls;

    if (status) {
        status->http_status = g_fake.status_post;
        status->curl_code = 0;
        status->tls_error = false;
    }
    g_fake.called_post = true;
    free(g_fake.last_request_body);
    g_fake.last_request_body = NULL;
    g_fake.last_request_len = 0;
    if (json_body) {
        size_t req_len = strlen(json_body);
        char* req = malloc(req_len + 1);
        if (!req) return false;
        memcpy(req, json_body, req_len);
        req[req_len] = '\0';
        g_fake.last_request_body = req;
        g_fake.last_request_len = req_len;
    }
    g_fake.headers_ok = g_fake.headers_ok && check_headers(headers, headers_count);
    g_fake.proxy_ok = g_fake.proxy_ok && check_proxy(proxy_url, no_proxy);
    if (g_fake.expected_url && strcmp(url, g_fake.expected_url) != 0) {
        g_fake.headers_ok = false;
    }
    if (g_fake.fail_post) {
        if (body) *body = NULL;
        if (len) *len = 0;
        if (status) status->http_status = 0;
        return false;
    }
    if (!g_fake.response_post) {
        if (body) *body = NULL;
        if (len) *len = 0;
        if (status) status->http_status = 0;
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
                      const char* proxy_url, const char* no_proxy, stream_cb cb, void* user_data,
                      llm_transport_status_t* status) {
    (void)timeout_ms;
    (void)read_idle_timeout_ms;
    (void)tls;

    if (status) {
        status->http_status = g_fake.status_stream;
        status->curl_code = 0;
        status->tls_error = false;
    }
    g_fake.called_stream = true;
    free(g_fake.last_request_body);
    g_fake.last_request_body = NULL;
    g_fake.last_request_len = 0;
    if (json_body) {
        size_t req_len = strlen(json_body);
        char* req = malloc(req_len + 1);
        if (!req) return false;
        memcpy(req, json_body, req_len);
        req[req_len] = '\0';
        g_fake.last_request_body = req;
        g_fake.last_request_len = req_len;
    }
    g_fake.headers_ok = g_fake.headers_ok && check_headers(headers, headers_count);
    g_fake.proxy_ok = g_fake.proxy_ok && check_proxy(proxy_url, no_proxy);
    if (g_fake.expected_url && strcmp(url, g_fake.expected_url) != 0) {
        g_fake.headers_ok = false;
    }
    if (g_fake.fail_stream) {
        if (status) status->http_status = 0;
        return false;
    }
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
        bool keep = cb(chunk, take, user_data);
        g_fake.stream_cb_calls++;
        g_fake.headers_ok = g_fake.headers_ok && check_headers(headers, headers_count);
        memset(chunk, 'x', take);
        if (!keep) return false;
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
    limits.max_frame_bytes = 1024;
    limits.max_sse_buffer_bytes = 64 * 1024;
    limits.max_tool_args_bytes_per_call = 1024;
    if (headers && headers_count > 0) {
        return llm_client_create_with_headers(base_url, &model, &timeout, &limits, headers, headers_count);
    }
    return llm_client_create(base_url, &model, &timeout, &limits);
}

static llm_client_t* make_client_with_embedding_limits(const char* base_url, size_t max_input_bytes,
                                                       size_t max_inputs) {
    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {0};
    timeout.connect_timeout_ms = 1000;
    timeout.overall_timeout_ms = 2000;
    timeout.read_idle_timeout_ms = 2000;
    llm_limits_t limits = {0};
    limits.max_response_bytes = 64 * 1024;
    limits.max_line_bytes = 1024;
    limits.max_frame_bytes = 1024;
    limits.max_sse_buffer_bytes = 64 * 1024;
    limits.max_tool_args_bytes_per_call = 1024;
    limits.max_embedding_input_bytes = max_input_bytes;
    limits.max_embedding_inputs = max_inputs;
    return llm_client_create(base_url, &model, &timeout, &limits);
}

static llm_client_t* make_client_with_stream_limits(const char* base_url, size_t max_line_bytes, size_t max_frame_bytes,
                                                    size_t max_sse_buffer_bytes) {
    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {0};
    timeout.connect_timeout_ms = 1000;
    timeout.overall_timeout_ms = 2000;
    timeout.read_idle_timeout_ms = 2000;
    llm_limits_t limits = {0};
    limits.max_response_bytes = 64 * 1024;
    limits.max_line_bytes = max_line_bytes;
    limits.max_frame_bytes = max_frame_bytes;
    limits.max_sse_buffer_bytes = max_sse_buffer_bytes;
    limits.max_tool_args_bytes_per_call = 1024;
    return llm_client_create(base_url, &model, &timeout, &limits);
}

static bool test_contract_body_ownership(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/chat/completions";
    g_fake.response_post = "{\"choices\":[{\"message\":{\"content\":\"hello\"},\"finish_reason\":\"stop\"}]}";

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

    llm_message_t msg = {0};
    msg.role = LLM_ROLE_USER;
    msg.content = "hi";
    msg.content_len = 2;

    llm_chat_result_t result;
    bool ok = llm_chat(client, &msg, 1, NULL, NULL, NULL, &result);
    if (!require(ok, "llm_chat failed")) return false;
    if (!require(result._internal == g_fake.last_body, "response buffer not transferred")) return false;
    if (!require(result.content, "missing content span")) return false;
    if (!require(result.finish_reason == LLM_FINISH_REASON_STOP, "finish reason mismatch")) return false;

    span_t expected = {0};
    if (!require(extract_choice_content(g_fake.last_body, g_fake.last_body_len, "message", &expected),
                 "content parse failed"))
        return false;
    if (!require(result.content_len == expected.len, "content length mismatch")) return false;
    if (!require(memcmp(result.content, expected.ptr, expected.len) == 0, "content mismatch")) return false;

    llm_chat_result_free(&result);
    llm_client_destroy(client);
    if (!require(g_fake.headers_ok, "headers unstable during non-stream request")) return false;
    return true;
}

static bool test_contract_chat_multi_choice_order(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/chat/completions";
    g_fake.response_post =
        "{\"choices\":[{\"message\":{\"content\":\"first\"},\"finish_reason\":\"stop\"},"
        "{\"message\":{\"content\":\"second\"},\"finish_reason\":\"stop\"}]}";

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

    llm_message_t msg = {0};
    msg.role = LLM_ROLE_USER;
    msg.content = "hi";
    msg.content_len = 2;

    llm_chat_result_t result;
    bool ok = llm_chat(client, &msg, 1, NULL, NULL, NULL, &result);
    if (!require(ok, "llm_chat failed")) return false;
    if (!require(result.choices_count == 2, "expected 2 chat choices")) return false;

    span_t expected0 = {0};
    span_t expected1 = {0};
    if (!require(extract_choice_content_at(g_fake.last_body, g_fake.last_body_len, 0, "message", &expected0),
                 "choice 0 parse failed"))
        return false;
    if (!require(extract_choice_content_at(g_fake.last_body, g_fake.last_body_len, 1, "message", &expected1),
                 "choice 1 parse failed"))
        return false;
    if (!require(result.choices[0].content_len == expected0.len, "choice 0 length mismatch")) return false;
    if (!require(memcmp(result.choices[0].content, expected0.ptr, expected0.len) == 0, "choice 0 mismatch"))
        return false;
    if (!require(result.choices[1].content_len == expected1.len, "choice 1 length mismatch")) return false;
    if (!require(memcmp(result.choices[1].content, expected1.ptr, expected1.len) == 0, "choice 1 mismatch"))
        return false;
    if (!require(result.content_len == expected0.len, "alias length mismatch")) return false;
    if (!require(memcmp(result.content, expected0.ptr, expected0.len) == 0, "alias mismatch")) return false;

    const llm_chat_choice_t* choice = NULL;
    if (!require(llm_chat_choice_get(&result, 1, &choice), "choice index 1 lookup failed")) return false;
    if (!require(choice == &result.choices[1], "choice pointer mismatch")) return false;
    choice = (const llm_chat_choice_t*)0x1;
    if (!require(!llm_chat_choice_get(&result, 2, &choice), "missing choice should fail")) return false;
    if (!require(choice == NULL, "missing choice should clear output")) return false;

    llm_chat_result_free(&result);
    llm_client_destroy(client);
    if (!require(g_fake.headers_ok, "headers unstable during chat choices")) return false;
    return true;
}

static bool test_contract_completions_multi_choice_order(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/completions";
    g_fake.response_post = "{\"choices\":[{\"text\":\"alpha\"},{\"text\":\"beta\"}]}";

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

    const char* prompt = "hi";
    llm_completions_result_t res = {0};
    bool ok = llm_completions(client, prompt, strlen(prompt), NULL, &res);
    if (!require(ok, "llm_completions failed")) return false;
    if (!require(res._internal == g_fake.last_body, "response buffer not transferred")) return false;
    if (!require(res.choices_count == 2, "expected 2 completion choices")) return false;

    span_t expected0 = {0};
    span_t expected1 = {0};
    if (!require(extract_completion_text_at(g_fake.last_body, g_fake.last_body_len, 0, &expected0),
                 "completion 0 parse failed"))
        return false;
    if (!require(extract_completion_text_at(g_fake.last_body, g_fake.last_body_len, 1, &expected1),
                 "completion 1 parse failed"))
        return false;
    if (!require(res.choices[0].text_len == expected0.len, "completion 0 length mismatch")) return false;
    if (!require(memcmp(res.choices[0].text, expected0.ptr, expected0.len) == 0, "completion 0 mismatch")) return false;
    if (!require(res.choices[1].text_len == expected1.len, "completion 1 length mismatch")) return false;
    if (!require(memcmp(res.choices[1].text, expected1.ptr, expected1.len) == 0, "completion 1 mismatch")) return false;

    const llm_completion_choice_t* choice = NULL;
    if (!require(llm_completions_choice_get(&res, 1, &choice), "completion index 1 lookup failed")) return false;
    if (!require(choice == &res.choices[1], "completion pointer mismatch")) return false;
    choice = (const llm_completion_choice_t*)0x1;
    if (!require(!llm_completions_choice_get(&res, 2, &choice), "missing completion should fail")) return false;
    if (!require(choice == NULL, "missing completion should clear output")) return false;

    llm_completions_free(&res);
    llm_client_destroy(client);
    if (!require(g_fake.headers_ok, "headers unstable during completions choices")) return false;
    return true;
}

static bool test_contract_model_switching(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/completions";
    g_fake.response_post = "{\"choices\":[{\"text\":\"ok\"}]}";

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

    llm_model_t model = {"next-model"};
    if (!require(llm_client_set_model(client, &model), "set model failed")) return false;

    const char* prompt = "hi";
    llm_completions_result_t res = {0};
    bool ok = llm_completions(client, prompt, strlen(prompt), NULL, &res);
    if (!require(ok, "llm_completions failed")) return false;
    if (!require(g_fake.last_request_body != NULL, "missing request body")) return false;

    span_t model_span = {0};
    if (!require(extract_string_field(g_fake.last_request_body, g_fake.last_request_len, "model", &model_span),
                 "missing model field"))
        return false;
    if (!require(model_span.len == strlen(model.name), "model length mismatch")) return false;
    if (!require(memcmp(model_span.ptr, model.name, model_span.len) == 0, "model mismatch")) return false;

    llm_completions_free(&res);
    llm_client_destroy(client);
    if (!require(g_fake.headers_ok, "headers unstable during model switch")) return false;
    return true;
}

static bool test_contract_streaming_headers(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/chat/completions";

    static const char stream_json[] = "{\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}";
    static const char stream_sse[] =
        "data: "
        "{\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}"
        "\n\n"
        "data: [DONE]\n\n";
    g_fake.stream_payload = stream_sse;
    g_fake.stream_payload_len = strlen(stream_sse);
    g_fake.stream_chunk_size = 16;

    const char* client_headers[] = {"X-Client: alpha"};
    llm_client_t* client = make_client("http://fake", client_headers, 1);
    if (!require(client, "client create failed")) return false;
    if (!require(llm_client_set_api_key(client, "token"), "set api key failed")) return false;

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
    if (!require(ok, "llm_chat_stream failed")) return false;
    if (!require(g_fake.stream_cb_calls > 0, "stream callback not invoked")) return false;
    if (!require(g_fake.headers_ok, "headers unstable during stream callbacks")) return false;
    if (!require(cap.finish_reason == LLM_FINISH_REASON_STOP, "finish reason mismatch")) return false;

    span_t expected = {0};
    if (!require(extract_choice_content(stream_json, strlen(stream_json), "delta", &expected),
                 "stream content parse failed"))
        return false;
    if (!require(cap.len == expected.len, "stream content length mismatch")) return false;
    if (!require(memcmp(cap.content, expected.ptr, expected.len) == 0, "stream content mismatch")) return false;

    llm_client_destroy(client);
    return true;
}

static bool test_contract_chat_stream_choice_index(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/chat/completions";

    static const char stream_json[] =
        "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"zero\"},\"finish_reason\":null},"
        "{\"index\":1,\"delta\":{\"content\":\"one\"},\"finish_reason\":\"stop\"}]}";
    static const char stream_sse[] =
        "data: "
        "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"zero\"},\"finish_reason\":null},"
        "{\"index\":1,\"delta\":{\"content\":\"one\"},\"finish_reason\":\"stop\"}]}"
        "\n\n"
        "data: [DONE]\n\n";
    g_fake.stream_payload = stream_sse;
    g_fake.stream_payload_len = strlen(stream_sse);
    g_fake.stream_chunk_size = 32;

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

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

    bool ok = llm_chat_stream_choice(client, &msg, 1, NULL, NULL, NULL, 1, &callbacks);
    if (!require(ok, "llm_chat_stream_choice failed")) return false;
    if (!require(g_fake.stream_cb_calls > 0, "stream callback not invoked")) return false;
    if (!require(cap.finish_reason == LLM_FINISH_REASON_STOP, "finish reason mismatch")) return false;

    span_t expected = {0};
    if (!require(extract_choice_content_at(stream_json, strlen(stream_json), 1, "delta", &expected),
                 "stream choice parse failed"))
        return false;
    if (!require(cap.len == expected.len, "stream content length mismatch")) return false;
    if (!require(memcmp(cap.content, expected.ptr, expected.len) == 0, "stream content mismatch")) return false;

    llm_client_destroy(client);
    if (!require(g_fake.headers_ok, "headers unstable during chat stream choice")) return false;
    return true;
}

static bool test_contract_completions_streaming_headers(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/completions";

    static const char stream_sse[] =
        "data: {\"choices\":[{\"text\":\"hi\",\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"text\":\"!\",\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    g_fake.stream_payload = stream_sse;
    g_fake.stream_payload_len = strlen(stream_sse);
    g_fake.stream_chunk_size = 12;

    const char* client_headers[] = {"X-Client: alpha"};
    llm_client_t* client = make_client("http://fake", client_headers, 1);
    if (!require(client, "client create failed")) return false;
    if (!require(llm_client_set_api_key(client, "token"), "set api key failed")) return false;

    const char* request_headers[] = {"X-Request: beta"};
    const char* expected_headers[] = {"X-Client: alpha", "Authorization: Bearer token", "X-Request: beta"};
    g_fake.expected_headers = expected_headers;
    g_fake.expected_headers_count = 3;

    struct stream_capture cap = {0};
    cap.finish_reason = LLM_FINISH_REASON_UNKNOWN;

    llm_stream_callbacks_t callbacks = {0};
    callbacks.user_data = &cap;
    callbacks.on_content_delta = on_content_delta;
    callbacks.on_finish_reason = on_finish_reason;

    const char* prompt = "ping";
    bool ok = llm_completions_stream_with_headers(client, prompt, strlen(prompt), NULL, &callbacks, request_headers, 1);
    if (!require(ok, "llm_completions_stream failed")) return false;
    if (!require(g_fake.stream_cb_calls > 0, "stream callback not invoked")) return false;
    if (!require(g_fake.headers_ok, "headers unstable during completions stream callbacks")) return false;
    if (!require(cap.finish_reason == LLM_FINISH_REASON_STOP, "finish reason mismatch")) return false;
    if (!require(cap.len == 3, "stream content length mismatch")) return false;
    if (!require(memcmp(cap.content, "hi!", 3) == 0, "stream content mismatch")) return false;

    llm_client_destroy(client);
    return true;
}

static bool test_contract_completions_stream_choice_index(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/completions";

    static const char stream_json[] =
        "{\"choices\":[{\"index\":0,\"text\":\"zero\",\"finish_reason\":null},"
        "{\"index\":1,\"text\":\"one\",\"finish_reason\":\"stop\"}]}";
    static const char stream_sse[] =
        "data: "
        "{\"choices\":[{\"index\":0,\"text\":\"zero\",\"finish_reason\":null},"
        "{\"index\":1,\"text\":\"one\",\"finish_reason\":\"stop\"}]}"
        "\n\n"
        "data: [DONE]\n\n";
    g_fake.stream_payload = stream_sse;
    g_fake.stream_payload_len = strlen(stream_sse);
    g_fake.stream_chunk_size = 32;

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

    struct stream_capture cap = {0};
    cap.finish_reason = LLM_FINISH_REASON_UNKNOWN;

    llm_stream_callbacks_t callbacks = {0};
    callbacks.user_data = &cap;
    callbacks.on_content_delta = on_content_delta;
    callbacks.on_finish_reason = on_finish_reason;

    const char* prompt = "ping";
    bool ok = llm_completions_stream_choice(client, prompt, strlen(prompt), NULL, 1, &callbacks);
    if (!require(ok, "llm_completions_stream_choice failed")) return false;
    if (!require(g_fake.stream_cb_calls > 0, "stream callback not invoked")) return false;
    if (!require(cap.finish_reason == LLM_FINISH_REASON_STOP, "finish reason mismatch")) return false;

    span_t expected = {0};
    if (!require(extract_completion_text_at(stream_json, strlen(stream_json), 1, &expected),
                 "completion choice parse failed"))
        return false;
    if (!require(cap.len == expected.len, "stream content length mismatch")) return false;
    if (!require(memcmp(cap.content, expected.ptr, expected.len) == 0, "stream content mismatch")) return false;

    llm_client_destroy(client);
    if (!require(g_fake.headers_ok, "headers unstable during completions stream choice")) return false;
    return true;
}

static bool test_contract_stream_line_cap_overflow(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/chat/completions";

    static const char stream_sse[] = "data: 123456789";
    g_fake.stream_payload = stream_sse;
    g_fake.stream_payload_len = strlen(stream_sse);
    g_fake.stream_chunk_size = 5;

    llm_client_t* client = make_client_with_stream_limits("http://fake", 8, 0, 64);
    if (!require(client, "client create failed")) return false;

    llm_message_t msg = {0};
    msg.role = LLM_ROLE_USER;
    msg.content = "ping";
    msg.content_len = 4;

    struct stream_capture cap = {0};
    llm_stream_callbacks_t callbacks = {0};
    callbacks.user_data = &cap;
    callbacks.on_content_delta = on_content_delta;

    bool ok = llm_chat_stream(client, &msg, 1, NULL, NULL, NULL, &callbacks);
    if (!require(!ok, "llm_chat_stream should fail on line cap overflow")) return false;
    if (!require(g_fake.stream_cb_calls > 0, "stream callback not invoked")) return false;
    if (!require(cap.len == 0, "content should not be produced on overflow")) return false;

    llm_client_destroy(client);
    if (!require(g_fake.headers_ok, "headers unstable during line cap overflow")) return false;
    return true;
}

static bool test_contract_embeddings_request_and_parse(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/embeddings";
    g_fake.response_post = "{\"data\":[{\"embedding\":[0.1,0.2,0.3]},{\"embedding\":[-1,2]}]}";

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

    llm_embedding_input_t inputs[] = {
        {"alpha", 5},
        {"beta", 4},
    };
    llm_embeddings_result_t res = {0};
    bool ok = llm_embeddings(client, inputs, 2, "{\"user\":\"test\"}", &res);
    if (!require(ok, "llm_embeddings failed")) return false;
    if (!require(res._internal == g_fake.last_body, "response buffer not transferred")) return false;
    if (!require(res.data_count == 2, "expected 2 embeddings")) return false;

    if (!require(g_fake.last_request_body != NULL, "missing request body")) return false;
    span_t model = {0};
    if (!require(extract_string_field(g_fake.last_request_body, g_fake.last_request_len, "model", &model),
                 "missing model field"))
        return false;
    if (!require(model.len == 10 && memcmp(model.ptr, "test-model", 10) == 0, "model mismatch")) return false;

    span_t user = {0};
    if (!require(extract_string_field(g_fake.last_request_body, g_fake.last_request_len, "user", &user),
                 "missing user field"))
        return false;
    if (!require(user.len == 4 && memcmp(user.ptr, "test", 4) == 0, "user mismatch")) return false;

    span_t input0 = {0};
    span_t input1 = {0};
    if (!require(extract_embedding_input_at(g_fake.last_request_body, g_fake.last_request_len, 0, &input0),
                 "missing input[0]"))
        return false;
    if (!require(extract_embedding_input_at(g_fake.last_request_body, g_fake.last_request_len, 1, &input1),
                 "missing input[1]"))
        return false;
    if (!require(input0.len == inputs[0].text_len, "input[0] length mismatch")) return false;
    if (!require(memcmp(input0.ptr, inputs[0].text, inputs[0].text_len) == 0, "input[0] mismatch")) return false;
    if (!require(input1.len == inputs[1].text_len, "input[1] length mismatch")) return false;
    if (!require(memcmp(input1.ptr, inputs[1].text, inputs[1].text_len) == 0, "input[1] mismatch")) return false;

    span_t expected0 = {0};
    span_t expected1 = {0};
    if (!require(extract_embedding_span_at(g_fake.last_body, g_fake.last_body_len, 0, &expected0),
                 "embedding[0] parse failed"))
        return false;
    if (!require(extract_embedding_span_at(g_fake.last_body, g_fake.last_body_len, 1, &expected1),
                 "embedding[1] parse failed"))
        return false;
    if (!require(res.data[0].embedding_len == expected0.len, "embedding[0] length mismatch")) return false;
    if (!require(memcmp(res.data[0].embedding, expected0.ptr, expected0.len) == 0, "embedding[0] mismatch"))
        return false;
    if (!require(res.data[1].embedding_len == expected1.len, "embedding[1] length mismatch")) return false;
    if (!require(memcmp(res.data[1].embedding, expected1.ptr, expected1.len) == 0, "embedding[1] mismatch"))
        return false;

    llm_embeddings_free(&res);
    llm_client_destroy(client);
    if (!require(g_fake.headers_ok, "headers unstable during embeddings request")) return false;
    return true;
}

static bool test_contract_embeddings_limits(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/embeddings";
    g_fake.response_post = "{\"data\":[{\"embedding\":[0]}]}";

    llm_client_t* client = make_client_with_embedding_limits("http://fake", 8, 1);
    if (!require(client, "client create failed")) return false;

    llm_embedding_input_t inputs[] = {
        {"alpha", 5},
        {"beta", 4},
    };
    llm_embeddings_result_t res = {0};
    bool ok = llm_embeddings(client, inputs, 2, NULL, &res);
    if (!require(!ok, "llm_embeddings should fail on input count cap")) return false;
    if (!require(!g_fake.called_post, "transport should not run on input count cap")) return false;

    llm_client_destroy(client);

    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/embeddings";
    g_fake.response_post = "{\"data\":[{\"embedding\":[0]}]}";

    client = make_client_with_embedding_limits("http://fake", 3, 2);
    if (!require(client, "client create failed")) return false;

    llm_embedding_input_t long_input = {"alpha", 5};
    ok = llm_embeddings(client, &long_input, 1, NULL, &res);
    if (!require(!ok, "llm_embeddings should fail on input length cap")) return false;
    if (!require(!g_fake.called_post, "transport should not run on input length cap")) return false;

    llm_client_destroy(client);
    return true;
}

static bool test_contract_proxy_passthrough(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.proxy_ok = true;
    g_fake.expected_url = "http://fake/health";
    g_fake.response_get = "{\"ok\":true}";
    g_fake.expected_proxy_url = "http://proxy.local:8080";
    g_fake.expected_no_proxy = "127.0.0.1,localhost";

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;
    if (!require(llm_client_set_proxy(client, g_fake.expected_proxy_url), "set proxy failed")) return false;
    if (!require(llm_client_set_no_proxy(client, g_fake.expected_no_proxy), "set no-proxy failed")) return false;

    bool ok = llm_health(client);
    if (!require(ok, "llm_health failed")) return false;
    if (!require(g_fake.proxy_ok, "proxy config not passed through")) return false;

    llm_client_destroy(client);
    return true;
}

static bool test_contract_failure_propagation(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/props";
    g_fake.fail_get = true;

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

    const char* json = "stale";
    size_t len = 123;
    bool ok = llm_props_get(client, &json, &len);
    if (!require(!ok, "llm_props_get should fail")) return false;
    if (!require(json == NULL, "failure should clear body")) return false;
    if (!require(len == 0, "failure should clear length")) return false;

    g_fake.expected_url = "http://fake/v1/chat/completions";
    g_fake.fail_stream = true;
    llm_message_t msg = {0};
    msg.role = LLM_ROLE_USER;
    msg.content = "ping";
    msg.content_len = 4;
    llm_stream_callbacks_t callbacks = {0};
    ok = llm_chat_stream(client, &msg, 1, NULL, NULL, NULL, &callbacks);
    if (!require(!ok, "llm_chat_stream should fail")) return false;
    if (!require(g_fake.stream_cb_calls == 0, "stream callback should not run on failure")) return false;

    llm_client_destroy(client);
    if (!require(g_fake.headers_ok, "headers unstable during failure tests")) return false;
    return true;
}

static bool test_contract_header_injection_rejected(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/health";
    g_fake.response_get = "{\"ok\":true}";

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

    const char* bad_headers[] = {"X-Test: ok\r\nInjected: nope"};
    bool ok = llm_health_with_headers(client, bad_headers, 1);
    if (!require(!ok, "llm_health_with_headers should reject injected header")) return false;
    if (!require(!g_fake.called_get, "transport should not run with invalid header")) return false;

    llm_client_destroy(client);
    return true;
}

static bool test_contract_api_key_injection_rejected(void) {
    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

    bool ok = llm_client_set_api_key(client, "bad\r\nX-Evil: yes");
    if (!require(!ok, "api key should reject injected header")) return false;

    llm_client_destroy(client);
    return true;
}

static bool test_contract_completions_missing_choices(void) {
    fake_reset();
    g_fake.headers_ok = true;
    g_fake.expected_url = "http://fake/v1/completions";
    g_fake.response_post = "{\"error\":{\"message\":\"missing\"}}";

    llm_client_t* client = make_client("http://fake", NULL, 0);
    if (!require(client, "client create failed")) return false;

    const char* prompt = "hi";
    llm_completions_result_t res = {0};
    bool ok = llm_completions(client, prompt, strlen(prompt), NULL, &res);
    if (!require(!ok, "llm_completions should fail on missing choices")) return false;
    if (!require(res.choices == NULL, "choices should be NULL on failure")) return false;
    if (!require(res.choices_count == 0, "choices_count should be 0 on failure")) return false;
    if (!require(res._internal == NULL, "response buffer should be NULL on failure")) return false;

    llm_client_destroy(client);
    if (!require(g_fake.headers_ok, "headers unstable during completions")) return false;
    return true;
}

int main(void) {
    if (!test_contract_header_injection_rejected()) return 1;
    if (!test_contract_api_key_injection_rejected()) return 1;
    if (!test_contract_completions_missing_choices()) return 1;
    if (!test_contract_body_ownership()) return 1;
    if (!test_contract_chat_multi_choice_order()) return 1;
    if (!test_contract_completions_multi_choice_order()) return 1;
    if (!test_contract_model_switching()) return 1;
    if (!test_contract_streaming_headers()) return 1;
    if (!test_contract_chat_stream_choice_index()) return 1;
    if (!test_contract_completions_streaming_headers()) return 1;
    if (!test_contract_completions_stream_choice_index()) return 1;
    if (!test_contract_stream_line_cap_overflow()) return 1;
    if (!test_contract_embeddings_request_and_parse()) return 1;
    if (!test_contract_embeddings_limits()) return 1;
    if (!test_contract_proxy_passthrough()) return 1;
    if (!test_contract_failure_propagation()) return 1;
    printf("Transport contract tests passed.\n");
    return 0;
}
