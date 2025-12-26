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

static llm_client_t* make_client(bool enable_last_error) {
    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {0};
    timeout.connect_timeout_ms = 1000;
    timeout.overall_timeout_ms = 2000;
    timeout.read_idle_timeout_ms = 2000;
    if (!enable_last_error) {
        return llm_client_create("http://fake", &model, &timeout, NULL);
    }
    llm_client_init_opts_t opts = {0};
    opts.enable_last_error = true;
    return llm_client_create_opts("http://fake", &model, &timeout, NULL, &opts);
}

static bool test_last_error_opt_in(void) {
    fake_reset();
    g_fake.status_get = 401;
    g_fake.response_get =
        "{\"error\":{\"message\":\"missing api key\",\"type\":\"auth_error\",\"code\":\"missing_api_key\"}}";

    llm_client_t* client = make_client(false);
    if (!require(client != NULL, "client create")) return false;

    const char* json = NULL;
    size_t len = 0;
    bool ok = llm_props_get(client, &json, &len);
    if (!require(!ok, "props should fail")) return false;
    if (!require(json == NULL && len == 0, "props output cleared")) return false;

    const llm_error_detail_t* last = llm_client_last_error(client);
    if (!require(last == NULL, "last error disabled")) return false;

    llm_client_destroy(client);
    return true;
}

static bool test_last_error_capture_and_clear(void) {
    fake_reset();
    g_fake.status_get = 401;
    g_fake.response_get =
        "{\"error\":{\"message\":\"missing api key\",\"type\":\"auth_error\",\"code\":\"missing_api_key\"}}";

    llm_client_t* client = make_client(true);
    if (!require(client != NULL, "client create")) return false;

    const char* json = NULL;
    size_t len = 0;
    bool ok = llm_props_get(client, &json, &len);
    if (!require(!ok, "props should fail")) return false;

    const llm_error_detail_t* last = llm_client_last_error(client);
    if (!require(last != NULL, "last error enabled")) return false;
    if (!require(last->code == LLM_ERR_FAILED, "last error code")) return false;
    if (!require(last->stage == LLM_ERROR_STAGE_PROTOCOL, "last error stage")) return false;
    if (!require(last->has_http_status && last->http_status == 401, "last error status")) return false;
    if (!require(assert_error_detail_matches(last), "last error matches")) return false;

    g_fake.status_get = 200;
    g_fake.response_get = "{}";
    ok = llm_props_get(client, &json, &len);
    if (!require(ok, "props should succeed")) return false;

    last = llm_client_last_error(client);
    if (!require(last != NULL, "last error still available")) return false;
    if (!require(last->code == LLM_ERR_NONE, "last error cleared")) return false;
    if (!require(!last->has_http_status, "last error status cleared")) return false;
    if (!require(last->raw_body == NULL && last->raw_body_len == 0, "last error body cleared")) return false;

    llm_client_destroy(client);
    return true;
}

static bool test_last_error_with_detail(void) {
    fake_reset();
    g_fake.status_get = 401;
    g_fake.response_get =
        "{\"error\":{\"message\":\"missing api key\",\"type\":\"auth_error\",\"code\":\"missing_api_key\"}}";

    llm_client_t* client = make_client(true);
    if (!require(client != NULL, "client create")) return false;

    const char* json = NULL;
    size_t len = 0;
    llm_error_detail_t detail = {0};
    llm_error_t err = llm_props_get_ex(client, &json, &len, &detail);
    if (!require(err == LLM_ERR_FAILED, "props should fail")) return false;

    const llm_error_detail_t* last = llm_client_last_error(client);
    if (!require(last != NULL, "last error enabled")) return false;
    if (!require(assert_error_detail_matches(&detail), "detail matches")) return false;
    if (!require(assert_error_detail_matches(last), "last error matches")) return false;
    if (!require(detail.raw_body && last->raw_body && detail.raw_body != last->raw_body, "last error copy")) {
        return false;
    }

    llm_error_detail_free(&detail);
    llm_client_destroy(client);
    return true;
}

static bool test_last_error_per_client(void) {
    fake_reset();
    g_fake.status_get = 401;
    g_fake.response_get =
        "{\"error\":{\"message\":\"missing api key\",\"type\":\"auth_error\",\"code\":\"missing_api_key\"}}";

    llm_client_t* client_a = make_client(true);
    llm_client_t* client_b = make_client(true);
    if (!require(client_a != NULL && client_b != NULL, "client create")) return false;

    const char* json = NULL;
    size_t len = 0;
    bool ok = llm_props_get(client_a, &json, &len);
    if (!require(!ok, "props should fail")) return false;

    const llm_error_detail_t* last_a = llm_client_last_error(client_a);
    const llm_error_detail_t* last_b = llm_client_last_error(client_b);
    if (!require(last_a && last_b, "last error enabled")) return false;
    if (!require(last_a->code == LLM_ERR_FAILED, "client a error")) return false;
    if (!require(last_b->code == LLM_ERR_NONE, "client b untouched")) return false;

    llm_client_destroy(client_a);
    llm_client_destroy(client_b);
    return true;
}

int main(void) {
    if (!test_last_error_opt_in()) return 1;
    if (!test_last_error_capture_and_clear()) return 1;
    if (!test_last_error_with_detail()) return 1;
    if (!test_last_error_per_client()) return 1;
    printf("Last error tests passed.\n");
    return 0;
}
