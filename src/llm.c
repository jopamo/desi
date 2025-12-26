#define _POSIX_C_SOURCE 200809L
#include "llm/llm.h"

#include "json_build.h"
#include "llm/internal.h"
#include "sse.h"
#include "tools_accum.h"
#include "transport_curl.h"
#define JSTOK_HEADER
#include <jstok.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct llm_client {
    char* base_url;
    llm_model_t model;
    llm_timeout_t timeout;
    llm_limits_t limits;
    char* auth_header;
    char* tls_ca_bundle_path;
    char* tls_ca_dir_path;
    char* tls_client_cert_path;
    char* tls_client_key_path;
    llm_tls_key_password_cb tls_key_password_cb;
    void* tls_key_password_user_data;
    bool tls_verify_peer;
    bool tls_verify_host;
    bool tls_insecure;
    char* proxy_url;
    char* no_proxy;
    char** headers;
    size_t headers_count;
    size_t custom_headers_count;
    size_t headers_cap;
    bool last_error_enabled;
    llm_error_detail_t last_error;
};

enum { LLM_ERROR_DETAIL_TOKENS_MAX = 64 };

void llm_client_destroy(llm_client_t* client);
static bool header_list_validate(const char* const* headers, size_t headers_count);
static bool header_has_crlf(const char* value);

static void error_detail_clear(llm_error_detail_t* detail) {
    if (!detail) return;
    memset(detail, 0, sizeof(*detail));
    detail->code = LLM_ERR_NONE;
    detail->stage = LLM_ERROR_STAGE_NONE;
}

void llm_error_detail_free(llm_error_detail_t* detail) {
    if (!detail) return;
    free(detail->_internal);
    error_detail_clear(detail);
}

const char* llm_errstr(llm_error_t code) {
    switch (code) {
        case LLM_ERR_NONE:
            return "none";
        case LLM_ERR_CANCELLED:
            return "cancelled";
        case LLM_ERR_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

static void error_detail_set_http_status(llm_error_detail_t* detail, long http_status) {
    if (!detail || http_status <= 0) return;
    detail->http_status = http_status;
    detail->has_http_status = true;
}

static void error_detail_parse_openai(llm_error_detail_t* detail, const char* json, size_t len) {
    if (!detail || !json || len == 0 || len > (size_t)INT_MAX) return;

    jstoktok_t tokens[LLM_ERROR_DETAIL_TOKENS_MAX];
    jstok_parser parser;
    jstok_init(&parser);
    int parsed = jstok_parse(&parser, json, (int)len, tokens, (int)LLM_ERROR_DETAIL_TOKENS_MAX);
    if (parsed <= 0) return;
    if (tokens[0].type != JSTOK_OBJECT) return;

    int error_idx = jstok_object_get(json, tokens, parsed, 0, "error");
    if (error_idx < 0 || tokens[error_idx].type != JSTOK_OBJECT) return;

    int msg_idx = jstok_object_get(json, tokens, parsed, error_idx, "message");
    if (msg_idx >= 0 && tokens[msg_idx].type == JSTOK_STRING) {
        jstok_span_t sp = jstok_span(json, &tokens[msg_idx]);
        detail->message = sp.p;
        detail->message_len = sp.n;
    }
    int type_idx = jstok_object_get(json, tokens, parsed, error_idx, "type");
    if (type_idx >= 0 && tokens[type_idx].type == JSTOK_STRING) {
        jstok_span_t sp = jstok_span(json, &tokens[type_idx]);
        detail->type = sp.p;
        detail->type_len = sp.n;
    }
    int code_idx = jstok_object_get(json, tokens, parsed, error_idx, "code");
    if (code_idx >= 0 && tokens[code_idx].type == JSTOK_STRING) {
        jstok_span_t sp = jstok_span(json, &tokens[code_idx]);
        detail->error_code = sp.p;
        detail->error_code_len = sp.n;
    }
}

static void error_detail_fill(llm_error_detail_t* detail, llm_error_t code, llm_error_stage_t stage, long http_status,
                              char* body, size_t body_len, bool parse_error) {
    if (!detail) {
        free(body);
        return;
    }
    llm_error_detail_free(detail);
    detail->code = code;
    detail->stage = stage;
    error_detail_set_http_status(detail, http_status);
    if (body) {
        detail->_internal = body;
        detail->raw_body = body;
        detail->raw_body_len = body_len;
        if (parse_error) {
            error_detail_parse_openai(detail, body, body_len);
        }
    }
}

static void last_error_reset(llm_client_t* client) {
    if (!client || !client->last_error_enabled) return;
    llm_error_detail_free(&client->last_error);
}

static void error_detail_capture(llm_client_t* client, llm_error_detail_t* detail, llm_error_t code,
                                 llm_error_stage_t stage, long http_status, char* body, size_t body_len,
                                 bool parse_error) {
    if (detail) {
        error_detail_fill(detail, code, stage, http_status, body, body_len, parse_error);
        if (client && client->last_error_enabled) {
            char* body_copy = NULL;
            size_t body_copy_len = 0;
            if (detail->raw_body && detail->raw_body_len > 0) {
                body_copy_len = detail->raw_body_len;
                body_copy = malloc(body_copy_len);
                if (body_copy) {
                    memcpy(body_copy, detail->raw_body, body_copy_len);
                } else {
                    body_copy_len = 0;
                }
            }
            error_detail_fill(&client->last_error, code, stage, http_status, body_copy, body_copy_len, parse_error);
        }
        return;
    }
    if (client && client->last_error_enabled) {
        error_detail_fill(&client->last_error, code, stage, http_status, body, body_len, parse_error);
        return;
    }
    free(body);
}

static void last_error_set_simple_if_empty(llm_client_t* client, llm_error_t code, llm_error_stage_t stage) {
    if (!client || !client->last_error_enabled) return;
    if (client->last_error.code != LLM_ERR_NONE) return;
    error_detail_fill(&client->last_error, code, stage, 0, NULL, 0, false);
}

static llm_error_stage_t transport_stage(const llm_transport_status_t* status) {
    if (status && status->tls_error) return LLM_ERROR_STAGE_TLS;
    return LLM_ERROR_STAGE_TRANSPORT;
}

static void llm_client_headers_free(llm_client_t* client) {
    if (!client) return;
    if (client->headers) {
        for (size_t i = 0; i < client->custom_headers_count; i++) {
            free(client->headers[i]);
        }
        free(client->headers);
    }
    client->headers = NULL;
    client->headers_count = 0;
    client->custom_headers_count = 0;
    client->headers_cap = 0;
}

static bool llm_client_headers_init(llm_client_t* client, const char* const* headers, size_t headers_count) {
    if (!header_list_validate(headers, headers_count)) return false;

    if (headers_count == 0) {
        client->headers = NULL;
        client->headers_count = 0;
        client->custom_headers_count = 0;
        client->headers_cap = 0;
        return true;
    }

    client->headers_cap = headers_count + 1;
    client->headers = calloc(client->headers_cap, sizeof(char*));
    if (!client->headers) return false;

    for (size_t i = 0; i < headers_count; i++) {
        if (!headers[i]) {
            llm_client_headers_free(client);
            return false;
        }
        client->headers[i] = strdup(headers[i]);
        if (!client->headers[i]) {
            llm_client_headers_free(client);
            return false;
        }
        client->custom_headers_count++;
        client->headers_count++;
    }

    return true;
}

llm_client_t* llm_client_create_with_headers_opts(const char* base_url, const llm_model_t* model,
                                                  const llm_timeout_t* timeout, const llm_limits_t* limits,
                                                  const char* const* headers, size_t headers_count,
                                                  const llm_client_init_opts_t* opts) {
    llm_client_t* client = malloc(sizeof(*client));
    if (!client) return NULL;
    memset(client, 0, sizeof(*client));

    client->base_url = strdup(base_url);
    if (model) {
        client->model.name = strdup(model->name);
    }
    if (timeout) {
        client->timeout = *timeout;
    } else {
        client->timeout.connect_timeout_ms = 10000;
        client->timeout.overall_timeout_ms = 60000;
    }
    if (limits) {
        client->limits = *limits;
    } else {
        client->limits.max_response_bytes = 10 * 1024 * 1024;
        client->limits.max_line_bytes = 1024 * 1024;
        client->limits.max_frame_bytes = 1024 * 1024;
        client->limits.max_sse_buffer_bytes = 10 * 1024 * 1024;
        client->limits.max_tool_args_bytes_per_call = 1024 * 1024;
        client->limits.max_tool_args_bytes_per_turn = 1024 * 1024;
        client->limits.max_tool_output_bytes_total = 1024 * 1024;
        client->limits.max_embedding_input_bytes = 1024 * 1024;
        client->limits.max_embedding_inputs = 1024;
        client->limits.max_content_parts = 128;
        client->limits.max_content_bytes = 1024 * 1024;
    }
    client->tls_verify_peer = true;
    client->tls_verify_host = true;
    client->last_error_enabled = opts && opts->enable_last_error;

    if (!llm_client_headers_init(client, headers, headers_count)) {
        llm_client_destroy(client);
        return NULL;
    }

    return client;
}

llm_client_t* llm_client_create_with_headers(const char* base_url, const llm_model_t* model,
                                             const llm_timeout_t* timeout, const llm_limits_t* limits,
                                             const char* const* headers, size_t headers_count) {
    return llm_client_create_with_headers_opts(base_url, model, timeout, limits, headers, headers_count, NULL);
}

llm_client_t* llm_client_create_opts(const char* base_url, const llm_model_t* model, const llm_timeout_t* timeout,
                                     const llm_limits_t* limits, const llm_client_init_opts_t* opts) {
    return llm_client_create_with_headers_opts(base_url, model, timeout, limits, NULL, 0, opts);
}

llm_client_t* llm_client_create(const char* base_url, const llm_model_t* model, const llm_timeout_t* timeout,
                                const llm_limits_t* limits) {
    return llm_client_create_with_headers_opts(base_url, model, timeout, limits, NULL, 0, NULL);
}

void llm_client_destroy(llm_client_t* client) {
    if (client) {
        free(client->base_url);
        free((char*)client->model.name);
        llm_client_headers_free(client);
        free(client->auth_header);
        free(client->tls_ca_bundle_path);
        free(client->tls_ca_dir_path);
        free(client->tls_client_cert_path);
        free(client->tls_client_key_path);
        free(client->proxy_url);
        free(client->no_proxy);
        llm_error_detail_free(&client->last_error);
        free(client);
    }
}

bool llm_client_set_model(llm_client_t* client, const llm_model_t* model) {
    if (!client || !model || !model->name) return false;
    if (client->model.name && strcmp(client->model.name, model->name) == 0) return true;

    char* name = strdup(model->name);
    if (!name) return false;

    free((char*)client->model.name);
    client->model.name = name;
    return true;
}

bool llm_client_set_api_key(llm_client_t* client, const char* api_key) {
    if (!client) return false;

    if (!api_key) {
        free(client->auth_header);
        client->auth_header = NULL;
        if (client->headers && client->headers_count > client->custom_headers_count) {
            client->headers[client->custom_headers_count] = NULL;
            client->headers_count = client->custom_headers_count;
        }
        return true;
    }

    if (header_has_crlf(api_key)) return false;

    const char* prefix = "Authorization: Bearer ";
    size_t prefix_len = strlen(prefix);
    size_t key_len = strlen(api_key);
    char* header = malloc(prefix_len + key_len + 1);
    if (!header) return false;
    memcpy(header, prefix, prefix_len);
    memcpy(header + prefix_len, api_key, key_len);
    header[prefix_len + key_len] = '\0';

    if (!client->headers) {
        size_t new_cap = client->custom_headers_count ? client->custom_headers_count + 1 : 1;
        client->headers = calloc(new_cap, sizeof(char*));
        if (!client->headers) {
            free(header);
            return false;
        }
        client->headers_cap = new_cap;
    } else if (client->headers_cap < client->custom_headers_count + 1) {
        size_t new_cap = client->custom_headers_count + 1;
        char** next = realloc(client->headers, new_cap * sizeof(char*));
        if (!next) {
            free(header);
            return false;
        }
        for (size_t i = client->headers_cap; i < new_cap; i++) {
            next[i] = NULL;
        }
        client->headers = next;
        client->headers_cap = new_cap;
    }

    free(client->auth_header);
    client->auth_header = header;
    client->headers[client->custom_headers_count] = client->auth_header;
    client->headers_count = client->custom_headers_count + 1;
    return true;
}

bool llm_client_set_tls_config(llm_client_t* client, const llm_tls_config_t* tls) {
    if (!client) return false;

    if (!tls) {
        free(client->tls_ca_bundle_path);
        free(client->tls_ca_dir_path);
        free(client->tls_client_cert_path);
        free(client->tls_client_key_path);
        client->tls_ca_bundle_path = NULL;
        client->tls_ca_dir_path = NULL;
        client->tls_client_cert_path = NULL;
        client->tls_client_key_path = NULL;
        client->tls_key_password_cb = NULL;
        client->tls_key_password_user_data = NULL;
        client->tls_verify_peer = true;
        client->tls_verify_host = true;
        client->tls_insecure = false;
        return true;
    }

    char* ca_path = NULL;
    if (tls->ca_bundle_path) {
        ca_path = strdup(tls->ca_bundle_path);
        if (!ca_path) return false;
    }
    char* ca_dir = NULL;
    if (tls->ca_dir_path) {
        ca_dir = strdup(tls->ca_dir_path);
        if (!ca_dir) {
            free(ca_path);
            return false;
        }
    }
    char* cert_path = NULL;
    if (tls->client_cert_path) {
        cert_path = strdup(tls->client_cert_path);
        if (!cert_path) {
            free(ca_path);
            free(ca_dir);
            return false;
        }
    }
    char* key_path = NULL;
    if (tls->client_key_path) {
        key_path = strdup(tls->client_key_path);
        if (!key_path) {
            free(ca_path);
            free(ca_dir);
            free(cert_path);
            return false;
        }
    }

    free(client->tls_ca_bundle_path);
    free(client->tls_ca_dir_path);
    free(client->tls_client_cert_path);
    free(client->tls_client_key_path);
    client->tls_ca_bundle_path = ca_path;
    client->tls_ca_dir_path = ca_dir;
    client->tls_client_cert_path = cert_path;
    client->tls_client_key_path = key_path;
    client->tls_key_password_cb = tls->key_password_cb;
    client->tls_key_password_user_data = tls->key_password_user_data;
    switch (tls->verify_peer) {
        case LLM_TLS_VERIFY_OFF:
            client->tls_verify_peer = false;
            break;
        case LLM_TLS_VERIFY_ON:
        case LLM_TLS_VERIFY_DEFAULT:
        default:
            client->tls_verify_peer = true;
            break;
    }
    switch (tls->verify_host) {
        case LLM_TLS_VERIFY_OFF:
            client->tls_verify_host = false;
            break;
        case LLM_TLS_VERIFY_ON:
        case LLM_TLS_VERIFY_DEFAULT:
        default:
            client->tls_verify_host = true;
            break;
    }
    client->tls_insecure = tls->insecure;
    return true;
}

bool llm_client_set_proxy(llm_client_t* client, const char* proxy_url) {
    if (!client) return false;

    if (!proxy_url || proxy_url[0] == '\0') {
        free(client->proxy_url);
        client->proxy_url = NULL;
        return true;
    }

    char* copy = strdup(proxy_url);
    if (!copy) return false;

    free(client->proxy_url);
    client->proxy_url = copy;
    return true;
}

bool llm_client_set_no_proxy(llm_client_t* client, const char* no_proxy_list) {
    if (!client) return false;

    if (!no_proxy_list || no_proxy_list[0] == '\0') {
        free(client->no_proxy);
        client->no_proxy = NULL;
        return true;
    }

    char* copy = strdup(no_proxy_list);
    if (!copy) return false;

    free(client->no_proxy);
    client->no_proxy = copy;
    return true;
}

const llm_error_detail_t* llm_client_last_error(const llm_client_t* client) {
    if (!client || !client->last_error_enabled) return NULL;
    return &client->last_error;
}

struct header_set {
    const char* const* headers;
    size_t count;
    const char** owned;
};

static void header_set_clear(struct header_set* set) {
    set->headers = NULL;
    set->count = 0;
    set->owned = NULL;
}

static void header_set_free(struct header_set* set) {
    if (!set) return;
    free((void*)set->owned);
    header_set_clear(set);
}

static bool header_has_crlf(const char* value) {
    if (!value) return false;
    for (const char* cur = value; *cur; cur++) {
        if (*cur == '\r' || *cur == '\n') return true;
    }
    return false;
}

static char header_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static bool header_name_span(const char* header, span_t* name) {
    if (!header) return false;
    const char* colon = strchr(header, ':');
    if (!colon) return false;
    const char* end = colon;
    while (end > header && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    if (end == header) return false;
    name->ptr = header;
    name->len = (size_t)(end - header);
    return true;
}

static bool header_name_eq(span_t a, span_t b) {
    if (a.len != b.len) return false;
    for (size_t i = 0; i < a.len; i++) {
        if (header_tolower(a.ptr[i]) != header_tolower(b.ptr[i])) return false;
    }
    return true;
}

static bool header_list_overrides(const char* const* headers, size_t headers_count, span_t name) {
    for (size_t i = 0; i < headers_count; i++) {
        span_t other;
        if (!header_name_span(headers[i], &other)) continue;
        if (header_name_eq(name, other)) return true;
    }
    return false;
}

static bool header_list_validate(const char* const* headers, size_t headers_count) {
    if (headers_count == 0) return true;
    if (!headers) return false;
    for (size_t i = 0; i < headers_count; i++) {
        if (!headers[i]) return false;
        if (header_has_crlf(headers[i])) return false;
        const char* colon = strchr(headers[i], ':');
        if (!colon || colon == headers[i]) return false;
    }
    return true;
}

static bool llm_header_set_init(struct header_set* set, const llm_client_t* client, const char* const* headers,
                                size_t headers_count) {
    header_set_clear(set);
    if (!header_list_validate(headers, headers_count)) return false;

    if (!client->headers || client->headers_count == 0) {
        set->headers = headers_count ? headers : NULL;
        set->count = headers_count;
        return true;
    }

    if (headers_count == 0) {
        set->headers = (const char* const*)client->headers;
        set->count = client->headers_count;
        return true;
    }

    size_t max_headers = client->headers_count + headers_count;
    const char** merged = malloc(max_headers * sizeof(*merged));
    if (!merged) return false;

    size_t out_count = 0;
    for (size_t i = 0; i < client->headers_count; i++) {
        span_t name;
        if (header_name_span(client->headers[i], &name) && header_list_overrides(headers, headers_count, name)) {
            continue;
        }
        merged[out_count++] = client->headers[i];
    }
    for (size_t i = 0; i < headers_count; i++) {
        merged[out_count++] = headers[i];
    }

    set->headers = merged;
    set->count = out_count;
    set->owned = merged;
    return true;
}

static const llm_tls_config_t* llm_client_tls_config(const llm_client_t* client, llm_tls_config_t* out) {
    out->ca_bundle_path = client->tls_ca_bundle_path;
    out->ca_dir_path = client->tls_ca_dir_path;
    out->client_cert_path = client->tls_client_cert_path;
    out->client_key_path = client->tls_client_key_path;
    out->key_password_cb = client->tls_key_password_cb;
    out->key_password_user_data = client->tls_key_password_user_data;
    out->verify_peer = client->tls_verify_peer ? LLM_TLS_VERIFY_ON : LLM_TLS_VERIFY_OFF;
    out->verify_host = client->tls_verify_host ? LLM_TLS_VERIFY_ON : LLM_TLS_VERIFY_OFF;
    out->insecure = client->tls_insecure;
    return out;
}

llm_error_t llm_health_with_headers_ex(llm_client_t* client, const char* const* headers, size_t headers_count,
                                       llm_error_detail_t* detail) {
    if (detail) llm_error_detail_free(detail);
    last_error_reset(client);
    char url[1024];
    snprintf(url, sizeof(url), "%s/health", client->base_url);
    char* body = NULL;
    size_t len = 0;
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    llm_transport_status_t status;
    bool ok = http_get(url, client->timeout.connect_timeout_ms, 1024, header_set.headers, header_set.count, tls_ptr,
                       client->proxy_url, client->no_proxy, &body, &len, &status);
    header_set_free(&header_set);
    if (!ok) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, transport_stage(&status), 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    if (status.http_status >= 400) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status, body, len,
                             true);
        return LLM_ERR_FAILED;
    }
    free(body);
    return LLM_ERR_NONE;
}

llm_error_t llm_health_ex(llm_client_t* client, llm_error_detail_t* detail) {
    return llm_health_with_headers_ex(client, NULL, 0, detail);
}

bool llm_health_with_headers(llm_client_t* client, const char* const* headers, size_t headers_count) {
    return llm_health_with_headers_ex(client, headers, headers_count, NULL) == LLM_ERR_NONE;
}

bool llm_health(llm_client_t* client) { return llm_health_with_headers(client, NULL, 0); }

llm_error_t llm_models_list_with_headers_ex(llm_client_t* client, char*** models, size_t* count,
                                            const char* const* headers, size_t headers_count,
                                            llm_error_detail_t* detail) {
    if (detail) llm_error_detail_free(detail);
    last_error_reset(client);
    if (!models || !count) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    *models = NULL;
    *count = 0;

    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/models", client->base_url);
    char* body = NULL;
    size_t len = 0;
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    llm_transport_status_t status;
    if (!http_get(url, client->timeout.connect_timeout_ms, client->limits.max_response_bytes, header_set.headers,
                  header_set.count, tls_ptr, client->proxy_url, client->no_proxy, &body, &len, &status)) {
        header_set_free(&header_set);
        error_detail_capture(client, detail, LLM_ERR_FAILED, transport_stage(&status), 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    header_set_free(&header_set);

    if (status.http_status >= 400) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status, body, len,
                             true);
        return LLM_ERR_FAILED;
    }

    jstoktok_t* tokens = NULL;
    int tok_count = 0;
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, body, (int)len, NULL, 0);
    if (needed <= 0) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_JSON, status.http_status, body, len, true);
        return LLM_ERR_FAILED;
    }
    tokens = malloc(needed * sizeof(jstoktok_t));
    if (!tokens) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_JSON, status.http_status, body, len, true);
        return LLM_ERR_FAILED;
    }
    jstok_init(&parser);
    if (jstok_parse(&parser, body, (int)len, tokens, needed) <= 0) {
        free(tokens);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_JSON, status.http_status, body, len, true);
        return LLM_ERR_FAILED;
    }
    tok_count = needed;

    if (tok_count == 0 || tokens[0].type != JSTOK_OBJECT) {
        free(tokens);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status, body, len,
                             true);
        return LLM_ERR_FAILED;
    }

    int data_idx = jstok_object_get(body, tokens, tok_count, 0, "data");
    if (data_idx < 0 || tokens[data_idx].type != JSTOK_ARRAY) {
        free(tokens);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status, body, len,
                             true);
        return LLM_ERR_FAILED;
    }

    int n = tokens[data_idx].size;
    if (n <= 0) {
        free(tokens);
        free(body);
        *models = NULL;
        *count = 0;
        return LLM_ERR_NONE;
    }

    char** out = malloc((size_t)n * sizeof(char*));
    if (!out) {
        free(tokens);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_JSON, status.http_status, body, len, true);
        return LLM_ERR_FAILED;
    }

    size_t out_count = 0;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        int m_idx = jstok_array_at(tokens, tok_count, data_idx, i);
        if (m_idx < 0 || tokens[m_idx].type != JSTOK_OBJECT) {
            ok = false;
            break;
        }
        int id_idx = jstok_object_get(body, tokens, tok_count, m_idx, "id");
        if (id_idx < 0 || tokens[id_idx].type != JSTOK_STRING) {
            ok = false;
            break;
        }
        jstok_span_t sp = jstok_span(body, &tokens[id_idx]);
        out[out_count] = malloc(sp.n + 1);
        if (!out[out_count]) {
            ok = false;
            break;
        }
        memcpy(out[out_count], sp.p, sp.n);
        out[out_count][sp.n] = '\0';
        out_count++;
    }

    free(tokens);

    if (!ok) {
        for (size_t i = 0; i < out_count; i++) {
            free(out[i]);
        }
        free(out);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status, body, len,
                             true);
        return LLM_ERR_FAILED;
    }

    free(body);
    *models = out;
    *count = out_count;
    return LLM_ERR_NONE;
}

llm_error_t llm_models_list_ex(llm_client_t* client, char*** models, size_t* count, llm_error_detail_t* detail) {
    return llm_models_list_with_headers_ex(client, models, count, NULL, 0, detail);
}

char** llm_models_list_with_headers(llm_client_t* client, size_t* count, const char* const* headers,
                                    size_t headers_count) {
    char** models = NULL;
    if (llm_models_list_with_headers_ex(client, &models, count, headers, headers_count, NULL) != LLM_ERR_NONE) {
        return NULL;
    }
    return models;
}

char** llm_models_list(llm_client_t* client, size_t* count) {
    return llm_models_list_with_headers(client, count, NULL, 0);
}

void llm_models_list_free(char** models, size_t count) {
    if (models) {
        for (size_t i = 0; i < count; i++) {
            free(models[i]);
        }
        free(models);
    }
}

llm_error_t llm_props_get_with_headers_ex(llm_client_t* client, const char** json, size_t* len,
                                          const char* const* headers, size_t headers_count,
                                          llm_error_detail_t* detail) {
    if (detail) llm_error_detail_free(detail);
    last_error_reset(client);
    if (!json || !len) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    *json = NULL;
    *len = 0;

    char url[1024];
    snprintf(url, sizeof(url), "%s/props", client->base_url);
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    llm_transport_status_t status;
    bool ok = http_get(url, client->timeout.connect_timeout_ms, client->limits.max_response_bytes, header_set.headers,
                       header_set.count, tls_ptr, client->proxy_url, client->no_proxy, (char**)json, len, &status);
    header_set_free(&header_set);
    if (!ok) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, transport_stage(&status), 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    if (status.http_status >= 400) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status, (char*)*json,
                             *len, true);
        *json = NULL;
        *len = 0;
        return LLM_ERR_FAILED;
    }
    return LLM_ERR_NONE;
}

llm_error_t llm_props_get_ex(llm_client_t* client, const char** json, size_t* len, llm_error_detail_t* detail) {
    return llm_props_get_with_headers_ex(client, json, len, NULL, 0, detail);
}

bool llm_props_get_with_headers(llm_client_t* client, const char** json, size_t* len, const char* const* headers,
                                size_t headers_count) {
    return llm_props_get_with_headers_ex(client, json, len, headers, headers_count, NULL) == LLM_ERR_NONE;
}

bool llm_props_get(llm_client_t* client, const char** json, size_t* len) {
    return llm_props_get_with_headers(client, json, len, NULL, 0);
}

// Forward declarations from other modules
int parse_chat_response(const char* json, size_t len, llm_chat_result_t* result);
int parse_chat_chunk(const char* json, size_t len, llm_chat_chunk_delta_t* delta, llm_usage_t* usage,
                     bool* usage_present);
int parse_chat_chunk_choice(const char* json, size_t len, size_t choice_index, llm_chat_chunk_delta_t* delta,
                            llm_usage_t* usage, bool* usage_present);
int parse_completions_response(const char* json, size_t len, llm_completions_result_t* result);
int parse_completions_chunk(const char* json, size_t len, span_t* text_delta, llm_finish_reason_t* finish_reason,
                            llm_usage_t* usage, bool* usage_present);
int parse_completions_chunk_choice(const char* json, size_t len, size_t choice_index, span_t* text_delta,
                                   llm_finish_reason_t* finish_reason, llm_usage_t* usage, bool* usage_present);
int parse_embeddings_response(const char* json, size_t len, llm_embeddings_result_t* result);

llm_error_t llm_completions_with_headers_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                            const char* params_json, llm_completions_result_t* result,
                                            const char* const* headers, size_t headers_count,
                                            llm_error_detail_t* detail) {
    if (detail) llm_error_detail_free(detail);
    last_error_reset(client);
    if (!result) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/completions", client->base_url);

    char* request_json = build_completions_request(client->model.name, prompt, prompt_len, false, false, params_json);
    if (!request_json) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }

    char* response_body = NULL;
    size_t response_len = 0;
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        free(request_json);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    llm_transport_status_t status;
    bool ok = http_post(url, request_json, client->timeout.overall_timeout_ms, client->limits.max_response_bytes,
                        header_set.headers, header_set.count, tls_ptr, client->proxy_url, client->no_proxy,
                        &response_body, &response_len, &status);
    header_set_free(&header_set);
    free(request_json);

    if (!ok) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, transport_stage(&status), 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    if (status.http_status >= 400) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status,
                             response_body, response_len, true);
        return LLM_ERR_FAILED;
    }

    memset(result, 0, sizeof(*result));
    int res = parse_completions_response(response_body, response_len, result);
    if (res < 0) {
        llm_error_stage_t stage = (res == LLM_PARSE_ERR_PROTOCOL) ? LLM_ERROR_STAGE_PROTOCOL : LLM_ERROR_STAGE_JSON;
        error_detail_capture(client, detail, LLM_ERR_FAILED, stage, status.http_status, response_body, response_len,
                             true);
        return LLM_ERR_FAILED;
    }
    result->_internal = response_body;
    return LLM_ERR_NONE;
}

llm_error_t llm_completions_ex(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                               llm_completions_result_t* result, llm_error_detail_t* detail) {
    return llm_completions_with_headers_ex(client, prompt, prompt_len, params_json, result, NULL, 0, detail);
}

bool llm_completions_with_headers(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                                  llm_completions_result_t* result, const char* const* headers, size_t headers_count) {
    return llm_completions_with_headers_ex(client, prompt, prompt_len, params_json, result, headers, headers_count,
                                           NULL) == LLM_ERR_NONE;
}

bool llm_completions(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                     llm_completions_result_t* result) {
    return llm_completions_with_headers(client, prompt, prompt_len, params_json, result, NULL, 0);
}

void llm_completions_free(llm_completions_result_t* result) {
    if (result) {
        free(result->choices);
        free(result->_internal);
        memset(result, 0, sizeof(*result));
    }
}

struct completions_stream_ctx {
    const llm_stream_callbacks_t* callbacks;
    size_t choice_index;
    bool include_usage;
};

static void on_sse_completions_data(void* user_data, span_t line) {
    struct completions_stream_ctx* ctx = user_data;
    span_t text_delta = {0};
    llm_finish_reason_t finish_reason = LLM_FINISH_REASON_UNKNOWN;
    llm_usage_t usage;
    bool usage_present = false;
    if (parse_completions_chunk_choice(line.ptr, line.len, ctx->choice_index, &text_delta, &finish_reason, &usage,
                                       &usage_present) == 0) {
        if (text_delta.ptr && ctx->callbacks->on_content_delta) {
            ctx->callbacks->on_content_delta(ctx->callbacks->user_data, text_delta.ptr, text_delta.len);
        }
        if (ctx->include_usage && usage_present && ctx->callbacks->on_usage) {
            ctx->callbacks->on_usage(ctx->callbacks->user_data, &usage);
        }
        if (finish_reason != LLM_FINISH_REASON_UNKNOWN && ctx->callbacks->on_finish_reason) {
            ctx->callbacks->on_finish_reason(ctx->callbacks->user_data, finish_reason);
        }
    }
}

struct sse_stream_ctx {
    sse_parser_t* sse;
    int sse_error;
    llm_abort_cb abort_cb;
    void* abort_user_data;
    llm_error_t error;
};

struct stream_capture_ctx {
    stream_cb inner;
    void* inner_user_data;
    struct growbuf buf;
    size_t max_bytes;
    bool capture;
};

static void stream_capture_init(struct stream_capture_ctx* cap, stream_cb inner, void* inner_user_data,
                                size_t max_bytes, bool enable) {
    memset(cap, 0, sizeof(*cap));
    cap->inner = inner;
    cap->inner_user_data = inner_user_data;
    cap->max_bytes = max_bytes;
    if (enable) {
        growbuf_init(&cap->buf, 4096);
        cap->capture = !cap->buf.nomem;
        if (!cap->capture) {
            growbuf_free(&cap->buf);
        }
    }
}

static bool stream_capture_cb(const char* chunk, size_t len, void* user_data) {
    struct stream_capture_ctx* cap = user_data;
    if (cap->capture) {
        if (!growbuf_append(&cap->buf, chunk, len, cap->max_bytes)) {
            growbuf_free(&cap->buf);
            cap->capture = false;
        }
    }
    return cap->inner(chunk, len, cap->inner_user_data);
}

static void stream_capture_release(struct stream_capture_ctx* cap, char** body, size_t* len) {
    if (!body || !len) return;
    *body = NULL;
    *len = 0;
    if (!cap->capture || !cap->buf.data) return;
    *body = cap->buf.data;
    *len = cap->buf.len;
    cap->buf.data = NULL;
    cap->buf.len = 0;
    cap->buf.cap = 0;
    cap->capture = false;
}

static bool on_sse_frame_abort(void* user_data) {
    struct sse_stream_ctx* cs = user_data;
    if (cs->abort_cb && cs->abort_cb(cs->abort_user_data)) {
        if (cs->error == LLM_ERR_NONE) {
            cs->error = LLM_ERR_CANCELLED;
        }
        return false;
    }
    return true;
}

static bool sse_stream_cb(const char* chunk, size_t len, void* user_data) {
    struct sse_stream_ctx* cs = user_data;
    if (cs->abort_cb && cs->abort_cb(cs->abort_user_data)) {
        if (cs->error == LLM_ERR_NONE) {
            cs->error = LLM_ERR_CANCELLED;
        }
        return false;
    }
    int rc = sse_feed(cs->sse, chunk, len);
    if (rc != SSE_OK) {
        cs->sse_error = rc;
        if (cs->error == LLM_ERR_NONE) {
            cs->error = (rc == SSE_ERR_ABORT) ? LLM_ERR_CANCELLED : LLM_ERR_FAILED;
        }
        return false;
    }
    if (cs->abort_cb && cs->abort_cb(cs->abort_user_data)) {
        if (cs->error == LLM_ERR_NONE) {
            cs->error = LLM_ERR_CANCELLED;
        }
        return false;
    }
    return true;
}

static llm_error_t llm_completions_stream_with_headers_choice_ex(
    llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json, size_t choice_index,
    const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb, void* abort_user_data, const char* const* headers,
    size_t headers_count, llm_error_detail_t* detail) {
    if (detail) llm_error_detail_free(detail);
    last_error_reset(client);
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/completions", client->base_url);

    const bool include_usage = callbacks && callbacks->include_usage;
    char* request_json =
        build_completions_request(client->model.name, prompt, prompt_len, true, include_usage, params_json);
    if (!request_json) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }

    struct completions_stream_ctx ctx = {callbacks, choice_index, include_usage};
    sse_parser_t* sse = sse_create(client->limits.max_line_bytes, client->limits.max_frame_bytes,
                                   client->limits.max_sse_buffer_bytes, client->limits.max_response_bytes);
    if (!sse) {
        free(request_json);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    sse_set_callback(sse, on_sse_completions_data, &ctx);

    struct sse_stream_ctx cs = {.sse = sse,
                                .sse_error = SSE_OK,
                                .abort_cb = abort_cb,
                                .abort_user_data = abort_user_data,
                                .error = LLM_ERR_NONE};
    sse_set_frame_callback(sse, on_sse_frame_abort, &cs);
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        sse_destroy(sse);
        free(request_json);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    struct stream_capture_ctx capture;
    stream_capture_init(&capture, sse_stream_cb, &cs, client->limits.max_response_bytes, detail != NULL);
    stream_cb cb = detail ? stream_capture_cb : sse_stream_cb;
    void* cb_user_data = detail ? (void*)&capture : (void*)&cs;
    llm_transport_status_t status;
    bool ok = http_post_stream(url, request_json, client->timeout.overall_timeout_ms,
                               client->timeout.read_idle_timeout_ms, header_set.headers, header_set.count, tls_ptr,
                               client->proxy_url, client->no_proxy, cb, cb_user_data, &status);
    header_set_free(&header_set);
    sse_destroy(sse);
    free(request_json);

    if (!ok) {
        llm_error_t err = (cs.error != LLM_ERR_NONE) ? cs.error : LLM_ERR_FAILED;
        llm_error_stage_t stage = LLM_ERROR_STAGE_TRANSPORT;
        if (err == LLM_ERR_CANCELLED) {
            stage = LLM_ERROR_STAGE_NONE;
        } else if (cs.sse_error != SSE_OK && cs.sse_error != SSE_ERR_ABORT) {
            stage = LLM_ERROR_STAGE_SSE;
        } else {
            stage = transport_stage(&status);
        }
        error_detail_capture(client, detail, err, stage, status.http_status, NULL, 0, false);
        growbuf_free(&capture.buf);
        return err;
    }
    if (cs.sse_error != SSE_OK) {
        llm_error_t err = (cs.error != LLM_ERR_NONE) ? cs.error : LLM_ERR_FAILED;
        llm_error_stage_t stage =
            (cs.sse_error == SSE_ERR_ABORT || err == LLM_ERR_CANCELLED) ? LLM_ERROR_STAGE_NONE : LLM_ERROR_STAGE_SSE;
        error_detail_capture(client, detail, err, stage, status.http_status, NULL, 0, false);
        growbuf_free(&capture.buf);
        return err;
    }
    if (status.http_status >= 400) {
        char* err_body = NULL;
        size_t err_len = 0;
        if (detail) {
            stream_capture_release(&capture, &err_body, &err_len);
        }
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status, err_body,
                             err_len, true);
        growbuf_free(&capture.buf);
        return LLM_ERR_FAILED;
    }
    growbuf_free(&capture.buf);
    return LLM_ERR_NONE;
}

bool llm_completions_stream_with_headers(llm_client_t* client, const char* prompt, size_t prompt_len,
                                         const char* params_json, const llm_stream_callbacks_t* callbacks,
                                         const char* const* headers, size_t headers_count) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, 0, callbacks, NULL,
                                                         NULL, headers, headers_count, NULL) == LLM_ERR_NONE;
}

bool llm_completions_stream(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                            const llm_stream_callbacks_t* callbacks) {
    return llm_completions_stream_with_headers(client, prompt, prompt_len, params_json, callbacks, NULL, 0);
}

bool llm_completions_stream_choice(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                                   size_t choice_index, const llm_stream_callbacks_t* callbacks) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, choice_index,
                                                         callbacks, NULL, NULL, NULL, 0, NULL) == LLM_ERR_NONE;
}

bool llm_completions_stream_choice_with_headers(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                const char* params_json, size_t choice_index,
                                                const llm_stream_callbacks_t* callbacks, const char* const* headers,
                                                size_t headers_count) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, choice_index,
                                                         callbacks, NULL, NULL, headers, headers_count,
                                                         NULL) == LLM_ERR_NONE;
}

llm_error_t llm_completions_stream_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                      const char* params_json, const llm_stream_callbacks_t* callbacks,
                                      llm_abort_cb abort_cb, void* abort_user_data) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, 0, callbacks,
                                                         abort_cb, abort_user_data, NULL, 0, NULL);
}

llm_error_t llm_completions_stream_with_headers_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                   const char* params_json, const llm_stream_callbacks_t* callbacks,
                                                   llm_abort_cb abort_cb, void* abort_user_data,
                                                   const char* const* headers, size_t headers_count) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, 0, callbacks,
                                                         abort_cb, abort_user_data, headers, headers_count, NULL);
}

llm_error_t llm_completions_stream_choice_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                             const char* params_json, size_t choice_index,
                                             const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb,
                                             void* abort_user_data) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, choice_index,
                                                         callbacks, abort_cb, abort_user_data, NULL, 0, NULL);
}

llm_error_t llm_completions_stream_choice_with_headers_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                          const char* params_json, size_t choice_index,
                                                          const llm_stream_callbacks_t* callbacks,
                                                          llm_abort_cb abort_cb, void* abort_user_data,
                                                          const char* const* headers, size_t headers_count) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, choice_index,
                                                         callbacks, abort_cb, abort_user_data, headers, headers_count,
                                                         NULL);
}

llm_error_t llm_completions_stream_detail_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                             const char* params_json, const llm_stream_callbacks_t* callbacks,
                                             llm_abort_cb abort_cb, void* abort_user_data, llm_error_detail_t* detail) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, 0, callbacks,
                                                         abort_cb, abort_user_data, NULL, 0, detail);
}

llm_error_t llm_completions_stream_with_headers_detail_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                          const char* params_json,
                                                          const llm_stream_callbacks_t* callbacks,
                                                          llm_abort_cb abort_cb, void* abort_user_data,
                                                          const char* const* headers, size_t headers_count,
                                                          llm_error_detail_t* detail) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, 0, callbacks,
                                                         abort_cb, abort_user_data, headers, headers_count, detail);
}

llm_error_t llm_completions_stream_choice_detail_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                    const char* params_json, size_t choice_index,
                                                    const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb,
                                                    void* abort_user_data, llm_error_detail_t* detail) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, choice_index,
                                                         callbacks, abort_cb, abort_user_data, NULL, 0, detail);
}

llm_error_t llm_completions_stream_choice_with_headers_detail_ex(
    llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json, size_t choice_index,
    const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb, void* abort_user_data, const char* const* headers,
    size_t headers_count, llm_error_detail_t* detail) {
    return llm_completions_stream_with_headers_choice_ex(client, prompt, prompt_len, params_json, choice_index,
                                                         callbacks, abort_cb, abort_user_data, headers, headers_count,
                                                         detail);
}

llm_error_t llm_embeddings_with_headers_ex(llm_client_t* client, const llm_embedding_input_t* inputs,
                                           size_t inputs_count, const char* params_json,
                                           llm_embeddings_result_t* result, const char* const* headers,
                                           size_t headers_count, llm_error_detail_t* detail) {
    if (detail) llm_error_detail_free(detail);
    last_error_reset(client);
    if (!result) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/embeddings", client->base_url);

    char* request_json =
        build_embeddings_request(client->model.name, inputs, inputs_count, params_json,
                                 client->limits.max_embedding_input_bytes, client->limits.max_embedding_inputs);
    if (!request_json) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }

    char* response_body = NULL;
    size_t response_len = 0;
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        free(request_json);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    llm_transport_status_t status;
    bool ok = http_post(url, request_json, client->timeout.overall_timeout_ms, client->limits.max_response_bytes,
                        header_set.headers, header_set.count, tls_ptr, client->proxy_url, client->no_proxy,
                        &response_body, &response_len, &status);
    header_set_free(&header_set);
    free(request_json);

    if (!ok) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, transport_stage(&status), 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    if (status.http_status >= 400) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status,
                             response_body, response_len, true);
        return LLM_ERR_FAILED;
    }

    memset(result, 0, sizeof(*result));
    int res = parse_embeddings_response(response_body, response_len, result);
    if (res < 0) {
        llm_error_stage_t stage = (res == LLM_PARSE_ERR_PROTOCOL) ? LLM_ERROR_STAGE_PROTOCOL : LLM_ERROR_STAGE_JSON;
        error_detail_capture(client, detail, LLM_ERR_FAILED, stage, status.http_status, response_body, response_len,
                             true);
        return LLM_ERR_FAILED;
    }
    result->_internal = response_body;
    return LLM_ERR_NONE;
}

llm_error_t llm_embeddings_ex(llm_client_t* client, const llm_embedding_input_t* inputs, size_t inputs_count,
                              const char* params_json, llm_embeddings_result_t* result, llm_error_detail_t* detail) {
    return llm_embeddings_with_headers_ex(client, inputs, inputs_count, params_json, result, NULL, 0, detail);
}

bool llm_embeddings_with_headers(llm_client_t* client, const llm_embedding_input_t* inputs, size_t inputs_count,
                                 const char* params_json, llm_embeddings_result_t* result, const char* const* headers,
                                 size_t headers_count) {
    return llm_embeddings_with_headers_ex(client, inputs, inputs_count, params_json, result, headers, headers_count,
                                          NULL) == LLM_ERR_NONE;
}

bool llm_embeddings(llm_client_t* client, const llm_embedding_input_t* inputs, size_t inputs_count,
                    const char* params_json, llm_embeddings_result_t* result) {
    return llm_embeddings_with_headers(client, inputs, inputs_count, params_json, result, NULL, 0);
}

void llm_embeddings_free(llm_embeddings_result_t* result) {
    if (result) {
        free(result->data);
        free(result->_internal);
        memset(result, 0, sizeof(*result));
    }
}

llm_error_t llm_chat_with_headers_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                     const char* params_json, const char* tooling_json,
                                     const char* response_format_json, llm_chat_result_t* result,
                                     const char* const* headers, size_t headers_count, llm_error_detail_t* detail) {
    if (detail) llm_error_detail_free(detail);
    last_error_reset(client);
    if (!result) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", client->base_url);

    char* request_json =
        build_chat_request(client->model.name, messages, messages_count, false, false, params_json, tooling_json,
                           response_format_json, client->limits.max_content_parts, client->limits.max_content_bytes);
    if (!request_json) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }

    char* response_body = NULL;
    size_t response_len = 0;
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        free(request_json);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    llm_transport_status_t status;
    bool ok = http_post(url, request_json, client->timeout.overall_timeout_ms, client->limits.max_response_bytes,
                        header_set.headers, header_set.count, tls_ptr, client->proxy_url, client->no_proxy,
                        &response_body, &response_len, &status);
    header_set_free(&header_set);
    free(request_json);

    if (!ok) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, transport_stage(&status), 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    if (status.http_status >= 400) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status,
                             response_body, response_len, true);
        return LLM_ERR_FAILED;
    }

    memset(result, 0, sizeof(*result));
    int res = parse_chat_response(response_body, response_len, result);
    if (res < 0) {
        llm_error_stage_t stage = (res == LLM_PARSE_ERR_PROTOCOL) ? LLM_ERROR_STAGE_PROTOCOL : LLM_ERROR_STAGE_JSON;
        error_detail_capture(client, detail, LLM_ERR_FAILED, stage, status.http_status, response_body, response_len,
                             true);
        return LLM_ERR_FAILED;
    }
    result->_internal = response_body;
    return LLM_ERR_NONE;
}

llm_error_t llm_chat_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                        const char* params_json, const char* tooling_json, const char* response_format_json,
                        llm_chat_result_t* result, llm_error_detail_t* detail) {
    return llm_chat_with_headers_ex(client, messages, messages_count, params_json, tooling_json, response_format_json,
                                    result, NULL, 0, detail);
}

bool llm_chat_with_headers(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                           const char* params_json, const char* tooling_json, const char* response_format_json,
                           llm_chat_result_t* result, const char* const* headers, size_t headers_count) {
    return llm_chat_with_headers_ex(client, messages, messages_count, params_json, tooling_json, response_format_json,
                                    result, headers, headers_count, NULL) == LLM_ERR_NONE;
}

bool llm_chat(llm_client_t* client, const llm_message_t* messages, size_t messages_count, const char* params_json,
              const char* tooling_json, const char* response_format_json, llm_chat_result_t* result) {
    return llm_chat_with_headers(client, messages, messages_count, params_json, tooling_json, response_format_json,
                                 result, NULL, 0);
}

void llm_chat_result_free(llm_chat_result_t* result) {
    if (result) {
        if (result->choices) {
            for (size_t i = 0; i < result->choices_count; i++) {
                free(result->choices[i].tool_calls);
            }
            free(result->choices);
        }
        free(result->_internal);
        memset(result, 0, sizeof(*result));
    }
}

bool llm_chat_choice_get(const llm_chat_result_t* result, size_t index, const llm_chat_choice_t** out_choice) {
    if (!out_choice) return false;
    *out_choice = NULL;
    if (!result || !result->choices || index >= result->choices_count) return false;
    *out_choice = &result->choices[index];
    return true;
}

bool llm_completions_choice_get(const llm_completions_result_t* result, size_t index,
                                const llm_completion_choice_t** out_choice) {
    if (!out_choice) return false;
    *out_choice = NULL;
    if (!result || !result->choices || index >= result->choices_count) return false;
    *out_choice = &result->choices[index];
    return true;
}

bool llm_tool_message_init(llm_message_t* msg, const char* content, size_t content_len, const char* tool_call_id,
                           size_t tool_call_id_len, const char* tool_name, size_t tool_name_len) {
    if (!msg) return false;
    if (!tool_call_id || tool_call_id_len == 0) return false;
    if (!content && content_len != 0) return false;
    if (!tool_name && tool_name_len != 0) return false;
    if (tool_name && tool_name_len == 0) return false;

    msg->role = LLM_ROLE_TOOL;
    msg->content = content;
    msg->content_len = content_len;
    msg->tool_call_id = tool_call_id;
    msg->tool_call_id_len = tool_call_id_len;
    msg->name = tool_name;
    msg->name_len = tool_name_len;
    msg->tool_calls_json = NULL;
    msg->tool_calls_json_len = 0;
    msg->content_json = NULL;
    msg->content_json_len = 0;
    return true;
}

static void llm_message_free_content(llm_message_t* msg) {
    if (msg) {
        free((char*)msg->content);
        free((char*)msg->tool_call_id);
        free((char*)msg->tool_calls_json);
        free((char*)msg->content_json);
        // Do not free name, it's not dynamically allocated in the tool loop
    }
}

enum { TOOL_LOOP_HASH_WINDOW = 8 };

struct tool_loop_guard {
    uint64_t recent_hashes[TOOL_LOOP_HASH_WINDOW];
    size_t recent_count;
    size_t recent_pos;
};

static void tool_loop_guard_init(struct tool_loop_guard* guard) { memset(guard, 0, sizeof(*guard)); }

static bool tool_loop_guard_seen(struct tool_loop_guard* guard, uint64_t hash) {
    for (size_t i = 0; i < guard->recent_count; i++) {
        if (guard->recent_hashes[i] == hash) return true;
    }
    guard->recent_hashes[guard->recent_pos] = hash;
    if (guard->recent_count < TOOL_LOOP_HASH_WINDOW) {
        guard->recent_count++;
    }
    guard->recent_pos = (guard->recent_pos + 1) % TOOL_LOOP_HASH_WINDOW;
    return false;
}

static uint64_t tool_loop_hash_bytes(uint64_t h, const void* data, size_t len) {
    const unsigned char* bytes = data;
    for (size_t i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t tool_loop_hash_u64(uint64_t h, uint64_t v) {
    for (size_t i = 0; i < 8; i++) {
        h ^= (unsigned char)(v & 0xffu);
        h *= 1099511628211ULL;
        v >>= 8;
    }
    return h;
}

static bool tool_loop_hash_turn(const llm_chat_result_t* result, uint64_t* out_hash) {
    if (!result || !out_hash) return false;
    uint64_t h = 1469598103934665603ULL;
    h = tool_loop_hash_u64(h, (uint64_t)result->tool_calls_count);
    for (size_t i = 0; i < result->tool_calls_count; i++) {
        const llm_tool_call_t* tc = &result->tool_calls[i];
        if (tc->name_len && !tc->name) return false;
        if (tc->arguments_len && !tc->arguments) return false;
        h = tool_loop_hash_u64(h, (uint64_t)tc->name_len);
        if (tc->name_len) {
            h = tool_loop_hash_bytes(h, tc->name, tc->name_len);
        }
        h = tool_loop_hash_u64(h, (uint64_t)tc->arguments_len);
        if (tc->arguments_len) {
            h = tool_loop_hash_bytes(h, tc->arguments, tc->arguments_len);
        }
    }
    if (result->content_len && !result->content) return false;
    if (result->reasoning_content_len && !result->reasoning_content) return false;
    h = tool_loop_hash_u64(h, (uint64_t)result->content_len);
    if (result->content_len) {
        h = tool_loop_hash_bytes(h, result->content, result->content_len);
    }
    h = tool_loop_hash_u64(h, (uint64_t)result->reasoning_content_len);
    if (result->reasoning_content_len) {
        h = tool_loop_hash_bytes(h, result->reasoning_content, result->reasoning_content_len);
    }
    *out_hash = h;
    return true;
}

static void tool_loop_free_history(llm_message_t** history, size_t* history_count) {
    if (!history || !*history) return;
    for (size_t i = 0; i < *history_count; i++) {
        llm_message_free_content(&(*history)[i]);
    }
    free(*history);
    *history = NULL;
    *history_count = 0;
}

struct stream_ctx {
    const llm_stream_callbacks_t* callbacks;
    size_t choice_index;
    struct tool_call_accumulator* accums;
    size_t accums_count;
    size_t max_tool_args;
    bool tool_calls_finalized;
    bool include_usage;
    bool protocol_error;
    llm_abort_cb abort_cb;
    void* abort_user_data;
    llm_error_t error;
};

static void stream_set_error(struct stream_ctx* ctx, llm_error_t err) {
    if (ctx->error == LLM_ERR_NONE) {
        ctx->error = err;
    }
}

static bool unescape_json_string_inplace(char* buf, size_t len, size_t* out_len) {
    if (!buf || !out_len) return false;
    if (len > (size_t)INT_MAX) return false;

    jstoktok_t tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = JSTOK_STRING;
    tok.start = 0;
    tok.end = (int)len;

    size_t unescaped_len = 0;
    if (jstok_unescape(buf, &tok, buf, len, &unescaped_len) != 0) return false;
    *out_len = unescaped_len;
    return true;
}

static bool validate_json_span(const char* json, size_t len) {
    if (!json || len == 0 || len > (size_t)INT_MAX) return false;

    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    return needed > 0;
}

static bool finalize_tool_calls(struct stream_ctx* ctx) {
    if (ctx->tool_calls_finalized) return true;
    ctx->tool_calls_finalized = true;

    for (size_t i = 0; i < ctx->accums_count; i++) {
        struct tool_call_accumulator* acc = &ctx->accums[i];
        if (!acc->active) continue;
        acc->frozen = true;
        if (!acc->saw_args) {
            stream_set_error(ctx, LLM_ERR_FAILED);
            return false;
        }
        size_t unescaped_len = 0;
        if (!unescape_json_string_inplace(acc->args_buf.data, acc->args_buf.len, &unescaped_len)) {
            stream_set_error(ctx, LLM_ERR_FAILED);
            return false;
        }
        acc->args_buf.len = unescaped_len;
        if (!validate_json_span(acc->args_buf.data, acc->args_buf.len)) {
            stream_set_error(ctx, LLM_ERR_FAILED);
            return false;
        }
        if (ctx->callbacks && ctx->callbacks->on_tool_args_complete) {
            ctx->callbacks->on_tool_args_complete(ctx->callbacks->user_data, i, acc->args_buf.data, acc->args_buf.len);
        }
    }

    return true;
}

static void on_sse_data(void* user_data, span_t line) {
    struct stream_ctx* ctx = user_data;
    if (ctx->protocol_error) return;
    llm_chat_chunk_delta_t delta;
    llm_usage_t usage;
    bool usage_present = false;
    if (parse_chat_chunk_choice(line.ptr, line.len, ctx->choice_index, &delta, &usage, &usage_present) == 0) {
        if (delta.content_delta && ctx->callbacks->on_content_delta) {
            ctx->callbacks->on_content_delta(ctx->callbacks->user_data, delta.content_delta, delta.content_delta_len);
        }
        if (delta.reasoning_delta && ctx->callbacks->on_reasoning_delta) {
            ctx->callbacks->on_reasoning_delta(ctx->callbacks->user_data, delta.reasoning_delta,
                                               delta.reasoning_delta_len);
        }
        if (ctx->include_usage && usage_present && ctx->callbacks->on_usage) {
            ctx->callbacks->on_usage(ctx->callbacks->user_data, &usage);
        }
        if (delta.tool_call_deltas_count > 0) {
            for (size_t i = 0; i < delta.tool_call_deltas_count; i++) {
                llm_tool_call_delta_t* td = &delta.tool_call_deltas[i];
                if (td->index >= ctx->accums_count) {
                    size_t new_count = td->index + 1;
                    struct tool_call_accumulator* next =
                        realloc(ctx->accums, new_count * sizeof(struct tool_call_accumulator));
                    if (!next) {
                        ctx->protocol_error = true;
                        stream_set_error(ctx, LLM_ERR_FAILED);
                        free(delta.tool_call_deltas);
                        return;
                    }
                    ctx->accums = next;
                    for (size_t j = ctx->accums_count; j < new_count; j++) {
                        accum_init(&ctx->accums[j]);
                    }
                    ctx->accums_count = new_count;
                }
                if (ctx->callbacks->on_tool_call_delta) {
                    ctx->callbacks->on_tool_call_delta(ctx->callbacks->user_data, td);
                }
                bool accum_ok = accum_feed_delta(&ctx->accums[td->index], td, ctx->max_tool_args);
                if (td->arguments_fragment && ctx->callbacks->on_tool_args_fragment) {
                    ctx->callbacks->on_tool_args_fragment(ctx->callbacks->user_data, td->index, td->arguments_fragment,
                                                          td->arguments_fragment_len);
                }
                if (!accum_ok) {
                    ctx->protocol_error = true;
                    stream_set_error(ctx, LLM_ERR_FAILED);
                    free(delta.tool_call_deltas);
                    return;
                }
            }
        }
        if (delta.finish_reason != LLM_FINISH_REASON_UNKNOWN) {
            if (delta.finish_reason == LLM_FINISH_REASON_TOOL_CALLS) {
                if (!finalize_tool_calls(ctx)) {
                    ctx->protocol_error = true;
                    free(delta.tool_call_deltas);
                    return;
                }
            }
            if (ctx->callbacks->on_finish_reason) {
                ctx->callbacks->on_finish_reason(ctx->callbacks->user_data, delta.finish_reason);
            }
        }
        free(delta.tool_call_deltas);
    }
}

static bool on_sse_frame_abort_stream(void* user_data) {
    struct stream_ctx* ctx = user_data;
    if (ctx->abort_cb && ctx->abort_cb(ctx->abort_user_data)) {
        stream_set_error(ctx, LLM_ERR_CANCELLED);
        return false;
    }
    return true;
}

typedef struct {
    sse_parser_t* sse;
    struct stream_ctx* ctx;
    int sse_error;
} curl_stream_ctx;

static bool curl_stream_cb(const char* chunk, size_t len, void* user_data) {
    curl_stream_ctx* cs = user_data;
    if (cs->ctx->abort_cb && cs->ctx->abort_cb(cs->ctx->abort_user_data)) {
        stream_set_error(cs->ctx, LLM_ERR_CANCELLED);
        return false;
    }
    if (cs->ctx->protocol_error) return false;
    int rc = sse_feed(cs->sse, chunk, len);
    if (rc != SSE_OK) {
        cs->sse_error = rc;
        if (rc == SSE_ERR_ABORT) {
            stream_set_error(cs->ctx, LLM_ERR_CANCELLED);
        } else {
            stream_set_error(cs->ctx, LLM_ERR_FAILED);
        }
        return false;
    }
    if (cs->ctx->abort_cb && cs->ctx->abort_cb(cs->ctx->abort_user_data)) {
        stream_set_error(cs->ctx, LLM_ERR_CANCELLED);
        return false;
    }
    if (cs->ctx->protocol_error) {
        stream_set_error(cs->ctx, LLM_ERR_FAILED);
        return false;
    }
    return true;
}

static llm_error_t llm_chat_stream_with_headers_choice_ex(llm_client_t* client, const llm_message_t* messages,
                                                          size_t messages_count, const char* params_json,
                                                          const char* tooling_json, const char* response_format_json,
                                                          size_t choice_index, const llm_stream_callbacks_t* callbacks,
                                                          llm_abort_cb abort_cb, void* abort_user_data,
                                                          const char* const* headers, size_t headers_count,
                                                          llm_error_detail_t* detail) {
    if (detail) llm_error_detail_free(detail);
    last_error_reset(client);
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", client->base_url);

    const bool include_usage = callbacks && callbacks->include_usage;
    char* request_json =
        build_chat_request(client->model.name, messages, messages_count, true, include_usage, params_json, tooling_json,
                           response_format_json, client->limits.max_content_parts, client->limits.max_content_bytes);
    if (!request_json) {
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }

    struct stream_ctx ctx = {.callbacks = callbacks,
                             .choice_index = choice_index,
                             .accums = NULL,
                             .accums_count = 0,
                             .max_tool_args = client->limits.max_tool_args_bytes_per_call,
                             .tool_calls_finalized = false,
                             .include_usage = include_usage,
                             .protocol_error = false,
                             .abort_cb = abort_cb,
                             .abort_user_data = abort_user_data,
                             .error = LLM_ERR_NONE};
    sse_parser_t* sse = sse_create(client->limits.max_line_bytes, client->limits.max_frame_bytes,
                                   client->limits.max_sse_buffer_bytes, client->limits.max_response_bytes);
    if (!sse) {
        free(request_json);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    sse_set_callback(sse, on_sse_data, &ctx);
    sse_set_frame_callback(sse, on_sse_frame_abort_stream, &ctx);

    curl_stream_ctx cs = {sse, &ctx, SSE_OK};
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        sse_destroy(sse);
        free(request_json);
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, 0, NULL, 0, false);
        return LLM_ERR_FAILED;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    struct stream_capture_ctx capture;
    stream_capture_init(&capture, curl_stream_cb, &cs, client->limits.max_response_bytes, detail != NULL);
    stream_cb cb = detail ? stream_capture_cb : curl_stream_cb;
    void* cb_user_data = detail ? (void*)&capture : (void*)&cs;
    llm_transport_status_t status;
    bool ok = http_post_stream(url, request_json, client->timeout.overall_timeout_ms,
                               client->timeout.read_idle_timeout_ms, header_set.headers, header_set.count, tls_ptr,
                               client->proxy_url, client->no_proxy, cb, cb_user_data, &status);
    header_set_free(&header_set);
    if (ok && !ctx.tool_calls_finalized && sse_is_done(sse)) {
        if (!finalize_tool_calls(&ctx)) {
            ctx.protocol_error = true;
            stream_set_error(&ctx, LLM_ERR_FAILED);
            ok = false;
        }
    }
    if (ctx.protocol_error) {
        ok = false;
    }

    for (size_t i = 0; i < ctx.accums_count; i++) {
        accum_free(&ctx.accums[i]);
    }
    free(ctx.accums);
    sse_destroy(sse);
    free(request_json);

    if (!ok) {
        llm_error_t err = (ctx.error != LLM_ERR_NONE) ? ctx.error : LLM_ERR_FAILED;
        llm_error_stage_t stage = LLM_ERROR_STAGE_TRANSPORT;
        if (err == LLM_ERR_CANCELLED) {
            stage = LLM_ERROR_STAGE_NONE;
        } else if (ctx.protocol_error) {
            stage = LLM_ERROR_STAGE_PROTOCOL;
        } else if (cs.sse_error != SSE_OK && cs.sse_error != SSE_ERR_ABORT) {
            stage = LLM_ERROR_STAGE_SSE;
        } else {
            stage = transport_stage(&status);
        }
        error_detail_capture(client, detail, err, stage, status.http_status, NULL, 0, false);
        growbuf_free(&capture.buf);
        return err;
    }
    if (cs.sse_error != SSE_OK) {
        llm_error_t err = (ctx.error != LLM_ERR_NONE) ? ctx.error : LLM_ERR_FAILED;
        llm_error_stage_t stage =
            (cs.sse_error == SSE_ERR_ABORT || err == LLM_ERR_CANCELLED) ? LLM_ERROR_STAGE_NONE : LLM_ERROR_STAGE_SSE;
        error_detail_capture(client, detail, err, stage, status.http_status, NULL, 0, false);
        growbuf_free(&capture.buf);
        return err;
    }
    if (ctx.error != LLM_ERR_NONE) {
        llm_error_stage_t stage = (ctx.error == LLM_ERR_CANCELLED) ? LLM_ERROR_STAGE_NONE : LLM_ERROR_STAGE_PROTOCOL;
        error_detail_capture(client, detail, ctx.error, stage, status.http_status, NULL, 0, false);
        growbuf_free(&capture.buf);
        return ctx.error;
    }
    if (status.http_status >= 400) {
        char* err_body = NULL;
        size_t err_len = 0;
        if (detail) {
            stream_capture_release(&capture, &err_body, &err_len);
        }
        error_detail_capture(client, detail, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL, status.http_status, err_body,
                             err_len, true);
        growbuf_free(&capture.buf);
        return LLM_ERR_FAILED;
    }
    growbuf_free(&capture.buf);
    return LLM_ERR_NONE;
}

bool llm_chat_stream_with_headers(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                  const char* params_json, const char* tooling_json, const char* response_format_json,
                                  const llm_stream_callbacks_t* callbacks, const char* const* headers,
                                  size_t headers_count) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, 0, callbacks, NULL, NULL, headers,
                                                  headers_count, NULL) == LLM_ERR_NONE;
}

bool llm_chat_stream(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                     const char* params_json, const char* tooling_json, const char* response_format_json,
                     const llm_stream_callbacks_t* callbacks) {
    return llm_chat_stream_with_headers(client, messages, messages_count, params_json, tooling_json,
                                        response_format_json, callbacks, NULL, 0);
}

bool llm_chat_stream_choice(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                            const char* params_json, const char* tooling_json, const char* response_format_json,
                            size_t choice_index, const llm_stream_callbacks_t* callbacks) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, choice_index, callbacks, NULL, NULL, NULL, 0,
                                                  NULL) == LLM_ERR_NONE;
}

bool llm_chat_stream_choice_with_headers(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                         const char* params_json, const char* tooling_json,
                                         const char* response_format_json, size_t choice_index,
                                         const llm_stream_callbacks_t* callbacks, const char* const* headers,
                                         size_t headers_count) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, choice_index, callbacks, NULL, NULL, headers,
                                                  headers_count, NULL) == LLM_ERR_NONE;
}

llm_error_t llm_chat_stream_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                               const char* params_json, const char* tooling_json, const char* response_format_json,
                               const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb, void* abort_user_data) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, 0, callbacks, abort_cb, abort_user_data, NULL,
                                                  0, NULL);
}

llm_error_t llm_chat_stream_with_headers_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                            const char* params_json, const char* tooling_json,
                                            const char* response_format_json, const llm_stream_callbacks_t* callbacks,
                                            llm_abort_cb abort_cb, void* abort_user_data, const char* const* headers,
                                            size_t headers_count) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, 0, callbacks, abort_cb, abort_user_data,
                                                  headers, headers_count, NULL);
}

llm_error_t llm_chat_stream_choice_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                      const char* params_json, const char* tooling_json,
                                      const char* response_format_json, size_t choice_index,
                                      const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb,
                                      void* abort_user_data) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, choice_index, callbacks, abort_cb,
                                                  abort_user_data, NULL, 0, NULL);
}

llm_error_t llm_chat_stream_choice_with_headers_ex(llm_client_t* client, const llm_message_t* messages,
                                                   size_t messages_count, const char* params_json,
                                                   const char* tooling_json, const char* response_format_json,
                                                   size_t choice_index, const llm_stream_callbacks_t* callbacks,
                                                   llm_abort_cb abort_cb, void* abort_user_data,
                                                   const char* const* headers, size_t headers_count) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, choice_index, callbacks, abort_cb,
                                                  abort_user_data, headers, headers_count, NULL);
}

llm_error_t llm_chat_stream_detail_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                      const char* params_json, const char* tooling_json,
                                      const char* response_format_json, const llm_stream_callbacks_t* callbacks,
                                      llm_abort_cb abort_cb, void* abort_user_data, llm_error_detail_t* detail) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, 0, callbacks, abort_cb, abort_user_data, NULL,
                                                  0, detail);
}

llm_error_t llm_chat_stream_with_headers_detail_ex(llm_client_t* client, const llm_message_t* messages,
                                                   size_t messages_count, const char* params_json,
                                                   const char* tooling_json, const char* response_format_json,
                                                   const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb,
                                                   void* abort_user_data, const char* const* headers,
                                                   size_t headers_count, llm_error_detail_t* detail) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, 0, callbacks, abort_cb, abort_user_data,
                                                  headers, headers_count, detail);
}

llm_error_t llm_chat_stream_choice_detail_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                             const char* params_json, const char* tooling_json,
                                             const char* response_format_json, size_t choice_index,
                                             const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb,
                                             void* abort_user_data, llm_error_detail_t* detail) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, choice_index, callbacks, abort_cb,
                                                  abort_user_data, NULL, 0, detail);
}

llm_error_t llm_chat_stream_choice_with_headers_detail_ex(llm_client_t* client, const llm_message_t* messages,
                                                          size_t messages_count, const char* params_json,
                                                          const char* tooling_json, const char* response_format_json,
                                                          size_t choice_index, const llm_stream_callbacks_t* callbacks,
                                                          llm_abort_cb abort_cb, void* abort_user_data,
                                                          const char* const* headers, size_t headers_count,
                                                          llm_error_detail_t* detail) {
    return llm_chat_stream_with_headers_choice_ex(client, messages, messages_count, params_json, tooling_json,
                                                  response_format_json, choice_index, callbacks, abort_cb,
                                                  abort_user_data, headers, headers_count, detail);
}

// Tool loop implementation is usually complex, let's put a simplified version here
llm_error_t llm_tool_loop_run_with_headers_ex(llm_client_t* client, const llm_message_t* initial_messages,
                                              size_t initial_count, const char* params_json, const char* tooling_json,
                                              const char* response_format_json, llm_tool_dispatch_cb dispatch,
                                              void* dispatch_user_data, llm_abort_cb abort_cb, void* abort_user_data,
                                              size_t max_turns, const char* const* headers, size_t headers_count) {
    last_error_reset(client);
    size_t history_count = initial_count;
    llm_message_t* history = calloc(history_count, sizeof(llm_message_t));
    if (!history) {
        last_error_set_simple_if_empty(client, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL);
        return LLM_ERR_FAILED;
    }

    for (size_t i = 0; i < initial_count; i++) {
        history[i].role = initial_messages[i].role;
        if (initial_messages[i].content_json_len && !initial_messages[i].content_json) {
            tool_loop_free_history(&history, &history_count);
            last_error_set_simple_if_empty(client, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL);
            return LLM_ERR_FAILED;
        }
        if (initial_messages[i].content && initial_messages[i].content_json) {
            tool_loop_free_history(&history, &history_count);
            last_error_set_simple_if_empty(client, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL);
            return LLM_ERR_FAILED;
        }
        if (initial_messages[i].content) {
            history[i].content = strdup(initial_messages[i].content);
            if (!history[i].content) {
                // Free already duplicated content and history itself
                for (size_t j = 0; j < i; j++) {
                    llm_message_free_content(&history[j]);
                }
                free(history);
                last_error_set_simple_if_empty(client, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL);
                return LLM_ERR_FAILED;
            }
            history[i].content_len = initial_messages[i].content_len;
        }
        if (initial_messages[i].content_json) {
            if (initial_messages[i].content_json_len == 0) {
                tool_loop_free_history(&history, &history_count);
                last_error_set_simple_if_empty(client, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL);
                return LLM_ERR_FAILED;
            }
            history[i].content_json = malloc(initial_messages[i].content_json_len);
            if (!history[i].content_json) {
                llm_message_free_content(&history[i]);
                for (size_t j = 0; j < i; j++) {
                    llm_message_free_content(&history[j]);
                }
                free(history);
                last_error_set_simple_if_empty(client, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL);
                return LLM_ERR_FAILED;
            }
            memcpy((char*)history[i].content_json, initial_messages[i].content_json,
                   initial_messages[i].content_json_len);
            history[i].content_json_len = initial_messages[i].content_json_len;
        }
        if (initial_messages[i].tool_call_id) {
            history[i].tool_call_id = strdup(initial_messages[i].tool_call_id);
            if (!history[i].tool_call_id) {
                llm_message_free_content(&history[i]);  // Free current content
                for (size_t j = 0; j < i; j++) {
                    llm_message_free_content(&history[j]);
                }
                free(history);
                last_error_set_simple_if_empty(client, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL);
                return LLM_ERR_FAILED;
            }
            history[i].tool_call_id_len = initial_messages[i].tool_call_id_len;
        }
        history[i].name =
            initial_messages[i].name;  // name is not duplicated as it's typically static or managed elsewhere
        history[i].name_len = initial_messages[i].name_len;
        if (initial_messages[i].tool_calls_json && initial_messages[i].tool_calls_json_len > 0) {
            history[i].tool_calls_json = malloc(initial_messages[i].tool_calls_json_len);
            if (!history[i].tool_calls_json) {
                llm_message_free_content(&history[i]);
                for (size_t j = 0; j < i; j++) {
                    llm_message_free_content(&history[j]);
                }
                free(history);
                last_error_set_simple_if_empty(client, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL);
                return LLM_ERR_FAILED;
            }
            memcpy((char*)history[i].tool_calls_json, initial_messages[i].tool_calls_json,
                   initial_messages[i].tool_calls_json_len);
            history[i].tool_calls_json_len = initial_messages[i].tool_calls_json_len;
        }
    }

    if (max_turns == 0) {
        tool_loop_free_history(&history, &history_count);
        last_error_set_simple_if_empty(client, LLM_ERR_FAILED, LLM_ERROR_STAGE_PROTOCOL);
        return LLM_ERR_FAILED;
    }

    struct tool_loop_guard guard;
    tool_loop_guard_init(&guard);
    size_t tool_output_total = 0;
    const size_t max_tool_args_per_turn = client->limits.max_tool_args_bytes_per_turn;
    const size_t max_tool_output_total = client->limits.max_tool_output_bytes_total;

    llm_error_t err = LLM_ERR_NONE;
    for (size_t turn = 0; turn < max_turns; turn++) {
        if (abort_cb && abort_cb(abort_user_data)) {
            err = LLM_ERR_CANCELLED;
            break;
        }
        llm_chat_result_t result;
        if (!llm_chat_with_headers(client, history, history_count, params_json, tooling_json, response_format_json,
                                   &result, headers, headers_count)) {
            err = LLM_ERR_FAILED;
            break;
        }

        if (result.finish_reason != LLM_FINISH_REASON_TOOL_CALLS) {
            llm_chat_result_free(&result);
            break;
        }
        if (result.tool_calls_count == 0 || !result.tool_calls_json || result.tool_calls_json_len == 0) {
            llm_chat_result_free(&result);
            err = LLM_ERR_FAILED;
            break;
        }
        if (turn + 1 >= max_turns) {
            llm_chat_result_free(&result);
            err = LLM_ERR_FAILED;
            break;
        }

        size_t turn_args_bytes = 0;
        bool turn_ok = true;
        for (size_t i = 0; i < result.tool_calls_count; i++) {
            const llm_tool_call_t* tc = &result.tool_calls[i];
            if (tc->name_len && !tc->name) {
                turn_ok = false;
                break;
            }
            if (tc->arguments_len && !tc->arguments) {
                turn_ok = false;
                break;
            }
            if (tc->arguments_len > SIZE_MAX - turn_args_bytes) {
                turn_ok = false;
                break;
            }
            turn_args_bytes += tc->arguments_len;
            if (max_tool_args_per_turn && turn_args_bytes > max_tool_args_per_turn) {
                turn_ok = false;
                break;
            }
        }
        if (!turn_ok) {
            llm_chat_result_free(&result);
            err = LLM_ERR_FAILED;
            break;
        }

        uint64_t turn_hash = 0;
        if (!tool_loop_hash_turn(&result, &turn_hash)) {
            llm_chat_result_free(&result);
            err = LLM_ERR_FAILED;
            break;
        }
        if (tool_loop_guard_seen(&guard, turn_hash)) {
            llm_chat_result_free(&result);
            err = LLM_ERR_FAILED;
            break;
        }

        // Prepare for new messages (assistant + tool results)
        size_t next_history_idx = history_count;
        size_t new_total_count = history_count + 1 + result.tool_calls_count;
        llm_message_t* new_history = realloc(history, new_total_count * sizeof(llm_message_t));
        if (!new_history) {
            llm_chat_result_free(&result);
            err = LLM_ERR_FAILED;
            break;
        }
        history = new_history;

        // Assistant message
        llm_message_t* assistant_msg = &history[next_history_idx++];
        memset(assistant_msg, 0, sizeof(*assistant_msg));
        assistant_msg->role = LLM_ROLE_ASSISTANT;
        assistant_msg->tool_calls_json = malloc(result.tool_calls_json_len);
        if (!assistant_msg->tool_calls_json) {
            llm_chat_result_free(&result);
            err = LLM_ERR_FAILED;
            break;
        }
        memcpy((char*)assistant_msg->tool_calls_json, result.tool_calls_json, result.tool_calls_json_len);
        assistant_msg->tool_calls_json_len = result.tool_calls_json_len;

        // Combine content and reasoning_content if available
        if (result.content || result.reasoning_content) {
            size_t total_len = 0;
            if (result.content) total_len += result.content_len;
            if (result.reasoning_content) total_len += result.reasoning_content_len;

            char* combined_content = malloc(total_len + 1);  // +1 for null terminator
            if (!combined_content) {
                // Handle allocation failure, clean up
                llm_message_free_content(assistant_msg);
                llm_chat_result_free(&result);
                err = LLM_ERR_FAILED;
                break;
            }

            size_t current_offset = 0;
            if (result.content) {
                memcpy(combined_content, result.content, result.content_len);
                current_offset += result.content_len;
            }
            if (result.reasoning_content) {
                memcpy(combined_content + current_offset, result.reasoning_content, result.reasoning_content_len);
                current_offset += result.reasoning_content_len;
            }
            combined_content[total_len] = '\0';

            assistant_msg->content = combined_content;
            assistant_msg->content_len = total_len;
        }
        history_count++;  // Increment history count for the assistant message

        // Handle tool call messages
        bool loop_error = false;
        for (size_t i = 0; i < result.tool_calls_count; i++) {
            char* res_json = NULL;
            size_t res_len = 0;

            // Call dispatch function
            if (!dispatch(dispatch_user_data, result.tool_calls[i].name, result.tool_calls[i].name_len,
                          result.tool_calls[i].arguments, result.tool_calls[i].arguments_len, &res_json, &res_len)) {
                // Dispatch failed. Clean up this tool result.
                if (res_json) free(res_json);
                err = LLM_ERR_FAILED;
                loop_error = true;
                break;
            }

            // If dispatch succeeded but returned no res_json, treat as failure for loop purposes
            if (!res_json) {
                err = LLM_ERR_FAILED;
                loop_error = true;
                break;
            }

            if (res_len > SIZE_MAX - tool_output_total) {
                free(res_json);
                err = LLM_ERR_FAILED;
                loop_error = true;
                break;
            }
            size_t next_output_total = tool_output_total + res_len;
            if (max_tool_output_total && next_output_total > max_tool_output_total) {
                free(res_json);
                err = LLM_ERR_FAILED;
                loop_error = true;
                break;
            }
            tool_output_total = next_output_total;

            llm_message_t* tool_msg = &history[next_history_idx++];
            memset(tool_msg, 0, sizeof(*tool_msg));
            tool_msg->role = LLM_ROLE_TOOL;
            tool_msg->content = res_json;  // Takes ownership of res_json
            tool_msg->content_len = res_len;

            if (result.tool_calls[i].id) {
                tool_msg->tool_call_id = strdup(result.tool_calls[i].id);
                if (!tool_msg->tool_call_id) {
                    // Allocation failure, clean up
                    llm_message_free_content(tool_msg);  // Free content (res_json)
                    err = LLM_ERR_FAILED;
                    loop_error = true;
                    break;
                }
                tool_msg->tool_call_id_len = result.tool_calls[i].id_len;
            }
            history_count++;  // Increment history count for each tool message
        }

        llm_chat_result_free(&result);
        if (loop_error) {
            break;
        }
    }

    // Free history copies if we made them
    if (err != LLM_ERR_NONE) {
        llm_error_stage_t stage = (err == LLM_ERR_CANCELLED) ? LLM_ERROR_STAGE_NONE : LLM_ERROR_STAGE_PROTOCOL;
        last_error_set_simple_if_empty(client, err, stage);
    }
    tool_loop_free_history(&history, &history_count);
    return err;
}

bool llm_tool_loop_run(llm_client_t* client, const llm_message_t* initial_messages, size_t initial_count,
                       const char* params_json, const char* tooling_json, const char* response_format_json,
                       llm_tool_dispatch_cb dispatch, void* dispatch_user_data, size_t max_turns) {
    return llm_tool_loop_run_with_headers_ex(client, initial_messages, initial_count, params_json, tooling_json,
                                             response_format_json, dispatch, dispatch_user_data, NULL, NULL, max_turns,
                                             NULL, 0) == LLM_ERR_NONE;
}

bool llm_tool_loop_run_with_headers(llm_client_t* client, const llm_message_t* initial_messages, size_t initial_count,
                                    const char* params_json, const char* tooling_json, const char* response_format_json,
                                    llm_tool_dispatch_cb dispatch, void* dispatch_user_data, size_t max_turns,
                                    const char* const* headers, size_t headers_count) {
    return llm_tool_loop_run_with_headers_ex(client, initial_messages, initial_count, params_json, tooling_json,
                                             response_format_json, dispatch, dispatch_user_data, NULL, NULL, max_turns,
                                             headers, headers_count) == LLM_ERR_NONE;
}

llm_error_t llm_tool_loop_run_ex(llm_client_t* client, const llm_message_t* initial_messages, size_t initial_count,
                                 const char* params_json, const char* tooling_json, const char* response_format_json,
                                 llm_tool_dispatch_cb dispatch, void* dispatch_user_data, llm_abort_cb abort_cb,
                                 void* abort_user_data, size_t max_turns) {
    return llm_tool_loop_run_with_headers_ex(client, initial_messages, initial_count, params_json, tooling_json,
                                             response_format_json, dispatch, dispatch_user_data, abort_cb,
                                             abort_user_data, max_turns, NULL, 0);
}
