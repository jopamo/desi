#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSTOK_HEADER
#include <jstok.h>

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
    const char* response_get;
    const char* response_post;
    const char* stream_payload;
    size_t stream_payload_len;
    long status_get;
    long status_post;
    long status_stream;
    bool fail_get;
    bool fail_post;
    bool fail_stream;
};

static struct fake_transport_state g_fake;

static void fake_reset(void) {
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.status_get = 200;
    g_fake.status_post = 200;
    g_fake.status_stream = 200;
}

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
        status->http_status = g_fake.status_get;
        status->curl_code = 0;
        status->tls_error = false;
    }
    if (g_fake.fail_get || !g_fake.response_get) {
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
    if (body) *body = resp;
    if (len) *len = resp_len;
    return true;
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
        status->http_status = g_fake.status_post;
        status->curl_code = 0;
        status->tls_error = false;
    }
    if (g_fake.fail_post || !g_fake.response_post) {
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
    if (body) *body = resp;
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
        status->http_status = g_fake.status_stream;
        status->curl_code = 0;
        status->tls_error = false;
    }
    if (g_fake.fail_stream || !g_fake.stream_payload || g_fake.stream_payload_len == 0) {
        if (status) status->http_status = 0;
        return false;
    }
    return cb(g_fake.stream_payload, g_fake.stream_payload_len, user_data);
}

static bool parse_error_field(const char* json, size_t len, const char* field, span_t* out) {
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return false;
    jstoktok_t* tokens = malloc((size_t)needed * sizeof(*tokens));
    if (!tokens) return false;
    jstok_init(&parser);
    int parsed = jstok_parse(&parser, json, (int)len, tokens, needed);
    if (parsed <= 0 || tokens[0].type != JSTOK_OBJECT) {
        free(tokens);
        return false;
    }
    int err_idx = obj_get_key(tokens, parsed, 0, json, "error");
    if (err_idx < 0 || tokens[err_idx].type != JSTOK_OBJECT) {
        free(tokens);
        return false;
    }
    int field_idx = obj_get_key(tokens, parsed, err_idx, json, field);
    if (field_idx < 0 || tokens[field_idx].type != JSTOK_STRING) {
        free(tokens);
        return false;
    }
    *out = tok_span(json, &tokens[field_idx]);
    free(tokens);
    return true;
}

static bool assert_error_detail_matches(const llm_error_detail_t* detail) {
    if (!detail || !detail->raw_body || detail->raw_body_len == 0) return false;
    span_t msg = {0};
    span_t type = {0};
    span_t code = {0};
    bool has_msg = parse_error_field(detail->raw_body, detail->raw_body_len, "message", &msg);
    bool has_type = parse_error_field(detail->raw_body, detail->raw_body_len, "type", &type);
    bool has_code = parse_error_field(detail->raw_body, detail->raw_body_len, "code", &code);
    if (has_msg) {
        if (!detail->message || detail->message_len != msg.len) return false;
        if (memcmp(detail->message, msg.ptr, msg.len) != 0) return false;
    } else if (detail->message || detail->message_len != 0) {
        return false;
    }
    if (has_type) {
        if (!detail->type || detail->type_len != type.len) return false;
        if (memcmp(detail->type, type.ptr, type.len) != 0) return false;
    } else if (detail->type || detail->type_len != 0) {
        return false;
    }
    if (has_code) {
        if (!detail->error_code || detail->error_code_len != code.len) return false;
        if (memcmp(detail->error_code, code.ptr, code.len) != 0) return false;
    } else if (detail->error_code || detail->error_code_len != 0) {
        return false;
    }
    return true;
}

static llm_client_t* make_client(const llm_limits_t* limits) {
    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {0};
    timeout.connect_timeout_ms = 1000;
    timeout.overall_timeout_ms = 2000;
    timeout.read_idle_timeout_ms = 2000;
    return llm_client_create("http://fake", &model, &timeout, limits);
}

static bool test_http_error_detail(void) {
    fake_reset();
    g_fake.status_get = 401;
    g_fake.response_get =
        "{\"error\":{\"message\":\"missing api key\",\"type\":\"auth_error\",\"code\":\"missing_api_key\"}}";

    llm_client_t* client = make_client(NULL);
    if (!require(client != NULL, "client create")) return false;

    const char* json = NULL;
    size_t len = 0;
    llm_error_detail_t detail = {0};
    llm_error_t err = llm_props_get_ex(client, &json, &len, &detail);
    if (!require(err == LLM_ERR_FAILED, "props should fail")) return false;
    if (!require(json == NULL && len == 0, "props output cleared")) return false;
    if (!require(detail.code == LLM_ERR_FAILED, "detail code")) return false;
    if (!require(detail.stage == LLM_ERROR_STAGE_PROTOCOL, "detail stage protocol")) return false;
    if (!require(detail.has_http_status && detail.http_status == 401, "detail http status")) return false;
    if (!require(assert_error_detail_matches(&detail), "detail fields match error JSON")) return false;
    llm_error_detail_free(&detail);
    llm_client_destroy(client);
    return true;
}

static bool test_malformed_error_body(void) {
    fake_reset();
    g_fake.status_get = 401;
    g_fake.response_get = "not-json";

    llm_client_t* client = make_client(NULL);
    if (!require(client != NULL, "client create")) return false;

    const char* json = NULL;
    size_t len = 0;
    llm_error_detail_t detail = {0};
    llm_error_t err = llm_props_get_ex(client, &json, &len, &detail);
    if (!require(err == LLM_ERR_FAILED, "props should fail on malformed error")) return false;
    if (!require(detail.raw_body && detail.raw_body_len == strlen(g_fake.response_get), "raw body present"))
        return false;

    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, detail.raw_body, (int)detail.raw_body_len, NULL, 0);
    if (!require(needed < 0, "malformed error should not parse")) return false;

    if (!require(detail.message == NULL && detail.type == NULL && detail.error_code == NULL,
                 "detail fields empty on malformed JSON")) {
        return false;
    }
    llm_error_detail_free(&detail);
    llm_client_destroy(client);
    return true;
}

static bool test_json_parse_error_stage(void) {
    fake_reset();
    g_fake.status_post = 200;
    g_fake.response_post = "{\"choices\": [";

    llm_client_t* client = make_client(NULL);
    if (!require(client != NULL, "client create")) return false;

    llm_completions_result_t result = {0};
    llm_error_detail_t detail = {0};
    llm_error_t err = llm_completions_ex(client, "hi", 2, NULL, &result, &detail);
    if (!require(err == LLM_ERR_FAILED, "completions should fail on invalid JSON")) return false;
    if (!require(detail.stage == LLM_ERROR_STAGE_JSON, "detail stage json")) return false;
    if (!require(detail.raw_body && detail.raw_body_len > 0, "raw body present")) return false;
    llm_error_detail_free(&detail);
    llm_client_destroy(client);
    return true;
}

static bool test_sse_error_stage(void) {
    fake_reset();
    g_fake.status_stream = 200;
    g_fake.stream_payload = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n";
    g_fake.stream_payload_len = strlen(g_fake.stream_payload);

    llm_limits_t limits = {0};
    limits.max_response_bytes = 1024;
    limits.max_line_bytes = 8;
    limits.max_frame_bytes = 1024;
    limits.max_sse_buffer_bytes = 1024;
    limits.max_tool_args_bytes_per_call = 1024;
    llm_client_t* client = make_client(&limits);
    if (!require(client != NULL, "client create")) return false;

    llm_stream_callbacks_t callbacks = {0};
    llm_error_detail_t detail = {0};
    llm_error_t err = llm_completions_stream_detail_ex(client, "hi", 2, NULL, &callbacks, NULL, NULL, &detail);
    if (!require(err == LLM_ERR_FAILED, "stream should fail on line overflow")) return false;
    if (!require(detail.stage == LLM_ERROR_STAGE_SSE, "detail stage sse")) return false;
    llm_error_detail_free(&detail);
    llm_client_destroy(client);
    return true;
}

int main(void) {
    if (!test_http_error_detail()) return 1;
    if (!test_malformed_error_body()) return 1;
    if (!test_json_parse_error_stage()) return 1;
    if (!test_sse_error_stage()) return 1;
    printf("Error detail tests passed.\n");
    return 0;
}
