#define _POSIX_C_SOURCE 200809L
#include "llm/llm.h"

#include "json_build.h"
#include "llm/internal.h"
#include "sse.h"
#include "tools_accum.h"
#include "transport_curl.h"
#define JSTOK_HEADER
#include <jstok.h>
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
};

void llm_client_destroy(llm_client_t* client);
static bool header_list_validate(const char* const* headers, size_t headers_count);
static bool header_has_crlf(const char* value);

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

llm_client_t* llm_client_create_with_headers(const char* base_url, const llm_model_t* model,
                                             const llm_timeout_t* timeout, const llm_limits_t* limits,
                                             const char* const* headers, size_t headers_count) {
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
        client->limits.max_embedding_input_bytes = 1024 * 1024;
        client->limits.max_embedding_inputs = 1024;
    }
    client->tls_verify_peer = true;
    client->tls_verify_host = true;

    if (!llm_client_headers_init(client, headers, headers_count)) {
        llm_client_destroy(client);
        return NULL;
    }

    return client;
}

llm_client_t* llm_client_create(const char* base_url, const llm_model_t* model, const llm_timeout_t* timeout,
                                const llm_limits_t* limits) {
    return llm_client_create_with_headers(base_url, model, timeout, limits, NULL, 0);
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
        free(client);
    }
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

bool llm_health_with_headers(llm_client_t* client, const char* const* headers, size_t headers_count) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/health", client->base_url);
    char* body = NULL;
    size_t len = 0;
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) return false;
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    bool ok = http_get(url, client->timeout.connect_timeout_ms, 1024, header_set.headers, header_set.count, tls_ptr,
                       client->proxy_url, client->no_proxy, &body, &len);
    header_set_free(&header_set);
    free(body);
    return ok;
}

bool llm_health(llm_client_t* client) { return llm_health_with_headers(client, NULL, 0); }

char** llm_models_list_with_headers(llm_client_t* client, size_t* count, const char* const* headers,
                                    size_t headers_count) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/models", client->base_url);
    char* body = NULL;
    size_t len = 0;
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) return NULL;
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    if (!http_get(url, client->timeout.connect_timeout_ms, client->limits.max_response_bytes, header_set.headers,
                  header_set.count, tls_ptr, client->proxy_url, client->no_proxy, &body, &len)) {
        header_set_free(&header_set);
        return NULL;
    }
    header_set_free(&header_set);

    jstoktok_t* tokens = NULL;
    int tok_count = 0;
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, body, (int)len, NULL, 0);
    if (needed <= 0) {
        free(body);
        return NULL;
    }
    tokens = malloc(needed * sizeof(jstoktok_t));
    jstok_init(&parser);
    jstok_parse(&parser, body, (int)len, tokens, needed);
    tok_count = needed;

    char** models = NULL;
    *count = 0;

    int data_idx = jstok_object_get(body, tokens, tok_count, 0, "data");
    if (data_idx >= 0 && tokens[data_idx].type == JSTOK_ARRAY) {
        int n = tokens[data_idx].size;
        models = malloc(n * sizeof(char*));
        for (int i = 0; i < n; i++) {
            int m_idx = jstok_array_at(tokens, tok_count, data_idx, i);
            if (m_idx >= 0 && tokens[m_idx].type == JSTOK_OBJECT) {
                int id_idx = jstok_object_get(body, tokens, tok_count, m_idx, "id");
                if (id_idx >= 0 && tokens[id_idx].type == JSTOK_STRING) {
                    jstok_span_t sp = jstok_span(body, &tokens[id_idx]);
                    models[*count] = malloc(sp.n + 1);
                    memcpy(models[*count], sp.p, sp.n);
                    models[*count][sp.n] = '\0';
                    (*count)++;
                }
            }
        }
    }

    free(tokens);
    free(body);
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

bool llm_props_get_with_headers(llm_client_t* client, const char** json, size_t* len, const char* const* headers,
                                size_t headers_count) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/props", client->base_url);
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) return false;
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    bool ok = http_get(url, client->timeout.connect_timeout_ms, client->limits.max_response_bytes, header_set.headers,
                       header_set.count, tls_ptr, client->proxy_url, client->no_proxy, (char**)json, len);
    header_set_free(&header_set);
    return ok;
}

bool llm_props_get(llm_client_t* client, const char** json, size_t* len) {
    return llm_props_get_with_headers(client, json, len, NULL, 0);
}

// Forward declarations from other modules
int parse_chat_response(const char* json, size_t len, llm_chat_result_t* result);
int parse_chat_chunk(const char* json, size_t len, llm_chat_chunk_delta_t* delta);
int parse_chat_chunk_choice(const char* json, size_t len, size_t choice_index, llm_chat_chunk_delta_t* delta);
int parse_completions_response(const char* json, size_t len, llm_completions_result_t* result);
int parse_completions_chunk(const char* json, size_t len, span_t* text_delta, llm_finish_reason_t* finish_reason);
int parse_completions_chunk_choice(const char* json, size_t len, size_t choice_index, span_t* text_delta,
                                   llm_finish_reason_t* finish_reason);
int parse_embeddings_response(const char* json, size_t len, llm_embeddings_result_t* result);

bool llm_completions_with_headers(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                                  llm_completions_result_t* result, const char* const* headers, size_t headers_count) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/completions", client->base_url);

    char* request_json = build_completions_request(client->model.name, prompt, prompt_len, false, params_json);
    if (!request_json) return false;

    char* response_body = NULL;
    size_t response_len = 0;
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        free(request_json);
        return false;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    bool ok = http_post(url, request_json, client->timeout.overall_timeout_ms, client->limits.max_response_bytes,
                        header_set.headers, header_set.count, tls_ptr, client->proxy_url, client->no_proxy,
                        &response_body, &response_len);
    header_set_free(&header_set);
    free(request_json);

    if (ok) {
        memset(result, 0, sizeof(*result));
        int res = parse_completions_response(response_body, response_len, result);
        if (res < 0) {
            free(response_body);
            ok = false;
        } else {
            result->_internal = response_body;
        }
    }
    return ok;
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
};

static void on_sse_completions_data(void* user_data, span_t line) {
    struct completions_stream_ctx* ctx = user_data;
    span_t text_delta = {0};
    llm_finish_reason_t finish_reason = LLM_FINISH_REASON_UNKNOWN;
    if (parse_completions_chunk_choice(line.ptr, line.len, ctx->choice_index, &text_delta, &finish_reason) == 0) {
        if (text_delta.ptr && ctx->callbacks->on_content_delta) {
            ctx->callbacks->on_content_delta(ctx->callbacks->user_data, text_delta.ptr, text_delta.len);
        }
        if (finish_reason != LLM_FINISH_REASON_UNKNOWN && ctx->callbacks->on_finish_reason) {
            ctx->callbacks->on_finish_reason(ctx->callbacks->user_data, finish_reason);
        }
    }
}

struct sse_stream_ctx {
    sse_parser_t* sse;
    int sse_error;
};

static bool sse_stream_cb(const char* chunk, size_t len, void* user_data) {
    struct sse_stream_ctx* cs = user_data;
    int rc = sse_feed(cs->sse, chunk, len);
    if (rc != SSE_OK) {
        cs->sse_error = rc;
        return false;
    }
    return true;
}

static bool llm_completions_stream_with_headers_choice(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                       const char* params_json, size_t choice_index,
                                                       const llm_stream_callbacks_t* callbacks,
                                                       const char* const* headers, size_t headers_count) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/completions", client->base_url);

    char* request_json = build_completions_request(client->model.name, prompt, prompt_len, true, params_json);
    if (!request_json) return false;

    struct completions_stream_ctx ctx = {callbacks, choice_index};
    sse_parser_t* sse = sse_create(client->limits.max_line_bytes, client->limits.max_frame_bytes,
                                   client->limits.max_sse_buffer_bytes, client->limits.max_response_bytes);
    sse_set_callback(sse, on_sse_completions_data, &ctx);

    struct sse_stream_ctx cs = {sse, SSE_OK};
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        sse_destroy(sse);
        free(request_json);
        return false;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    bool ok = http_post_stream(url, request_json, client->timeout.overall_timeout_ms,
                               client->timeout.read_idle_timeout_ms, header_set.headers, header_set.count, tls_ptr,
                               client->proxy_url, client->no_proxy, sse_stream_cb, &cs);
    if (cs.sse_error != SSE_OK) {
        ok = false;
    }
    header_set_free(&header_set);
    sse_destroy(sse);
    free(request_json);

    return ok;
}

bool llm_completions_stream_with_headers(llm_client_t* client, const char* prompt, size_t prompt_len,
                                         const char* params_json, const llm_stream_callbacks_t* callbacks,
                                         const char* const* headers, size_t headers_count) {
    return llm_completions_stream_with_headers_choice(client, prompt, prompt_len, params_json, 0, callbacks, headers,
                                                      headers_count);
}

bool llm_completions_stream(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                            const llm_stream_callbacks_t* callbacks) {
    return llm_completions_stream_with_headers(client, prompt, prompt_len, params_json, callbacks, NULL, 0);
}

bool llm_completions_stream_choice(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                                   size_t choice_index, const llm_stream_callbacks_t* callbacks) {
    return llm_completions_stream_with_headers_choice(client, prompt, prompt_len, params_json, choice_index, callbacks,
                                                      NULL, 0);
}

bool llm_completions_stream_choice_with_headers(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                const char* params_json, size_t choice_index,
                                                const llm_stream_callbacks_t* callbacks, const char* const* headers,
                                                size_t headers_count) {
    return llm_completions_stream_with_headers_choice(client, prompt, prompt_len, params_json, choice_index, callbacks,
                                                      headers, headers_count);
}

bool llm_embeddings_with_headers(llm_client_t* client, const llm_embedding_input_t* inputs, size_t inputs_count,
                                 const char* params_json, llm_embeddings_result_t* result, const char* const* headers,
                                 size_t headers_count) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/embeddings", client->base_url);

    char* request_json =
        build_embeddings_request(client->model.name, inputs, inputs_count, params_json,
                                 client->limits.max_embedding_input_bytes, client->limits.max_embedding_inputs);
    if (!request_json) return false;

    char* response_body = NULL;
    size_t response_len = 0;
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        free(request_json);
        return false;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    bool ok = http_post(url, request_json, client->timeout.overall_timeout_ms, client->limits.max_response_bytes,
                        header_set.headers, header_set.count, tls_ptr, client->proxy_url, client->no_proxy,
                        &response_body, &response_len);
    header_set_free(&header_set);
    free(request_json);

    if (ok) {
        memset(result, 0, sizeof(*result));
        int res = parse_embeddings_response(response_body, response_len, result);
        if (res < 0) {
            free(response_body);
            ok = false;
        } else {
            result->_internal = response_body;
        }
    }
    return ok;
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

bool llm_chat_with_headers(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                           const char* params_json, const char* tooling_json, const char* response_format_json,
                           llm_chat_result_t* result, const char* const* headers, size_t headers_count) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", client->base_url);

    char* request_json = build_chat_request(client->model.name, messages, messages_count, false, params_json,
                                            tooling_json, response_format_json);
    if (!request_json) return false;

    char* response_body = NULL;
    size_t response_len = 0;
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        free(request_json);
        return false;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    bool ok = http_post(url, request_json, client->timeout.overall_timeout_ms, client->limits.max_response_bytes,
                        header_set.headers, header_set.count, tls_ptr, client->proxy_url, client->no_proxy,
                        &response_body, &response_len);
    header_set_free(&header_set);
    free(request_json);

    if (ok) {
        memset(result, 0, sizeof(*result));
        int res = parse_chat_response(response_body, response_len, result);
        if (res < 0) {
            free(response_body);
            ok = false;
        } else {
            result->_internal = response_body;
        }
    }

    return ok;
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

static void llm_message_free_content(llm_message_t* msg) {
    if (msg) {
        free((char*)msg->content);
        free((char*)msg->tool_call_id);
        // Do not free name, it's not dynamically allocated in the tool loop
    }
}

struct stream_ctx {
    const llm_stream_callbacks_t* callbacks;
    size_t choice_index;
    struct tool_call_accumulator* accums;
    size_t accums_count;
    size_t max_tool_args;
};

static void on_sse_data(void* user_data, span_t line) {
    struct stream_ctx* ctx = user_data;
    llm_chat_chunk_delta_t delta;
    if (parse_chat_chunk_choice(line.ptr, line.len, ctx->choice_index, &delta) == 0) {
        if (delta.content_delta && ctx->callbacks->on_content_delta) {
            ctx->callbacks->on_content_delta(ctx->callbacks->user_data, delta.content_delta, delta.content_delta_len);
        }
        if (delta.reasoning_delta && ctx->callbacks->on_reasoning_delta) {
            ctx->callbacks->on_reasoning_delta(ctx->callbacks->user_data, delta.reasoning_delta,
                                               delta.reasoning_delta_len);
        }
        if (delta.tool_call_deltas_count > 0) {
            for (size_t i = 0; i < delta.tool_call_deltas_count; i++) {
                llm_tool_call_delta_t* td = &delta.tool_call_deltas[i];
                if (td->index >= ctx->accums_count) {
                    size_t new_count = td->index + 1;
                    ctx->accums = realloc(ctx->accums, new_count * sizeof(struct tool_call_accumulator));
                    for (size_t j = ctx->accums_count; j < new_count; j++) {
                        accum_init(&ctx->accums[j]);
                    }
                    ctx->accums_count = new_count;
                }
                accum_feed_delta(&ctx->accums[td->index], td, ctx->max_tool_args);
                if (td->arguments_fragment && ctx->callbacks->on_tool_args_fragment) {
                    ctx->callbacks->on_tool_args_fragment(ctx->callbacks->user_data, td->index, td->arguments_fragment,
                                                          td->arguments_fragment_len);
                }
            }
        }
        if (delta.finish_reason != LLM_FINISH_REASON_UNKNOWN && ctx->callbacks->on_finish_reason) {
            ctx->callbacks->on_finish_reason(ctx->callbacks->user_data, delta.finish_reason);
        }
        free(delta.tool_call_deltas);
    }
}

typedef struct {
    sse_parser_t* sse;
    struct stream_ctx* ctx;
    int sse_error;
} curl_stream_ctx;

static bool curl_stream_cb(const char* chunk, size_t len, void* user_data) {
    curl_stream_ctx* cs = user_data;
    int rc = sse_feed(cs->sse, chunk, len);
    if (rc != SSE_OK) {
        cs->sse_error = rc;
        return false;
    }
    return true;
}

static bool llm_chat_stream_with_headers_choice(llm_client_t* client, const llm_message_t* messages,
                                                size_t messages_count, const char* params_json,
                                                const char* tooling_json, const char* response_format_json,
                                                size_t choice_index, const llm_stream_callbacks_t* callbacks,
                                                const char* const* headers, size_t headers_count) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", client->base_url);

    char* request_json = build_chat_request(client->model.name, messages, messages_count, true, params_json,
                                            tooling_json, response_format_json);
    if (!request_json) return false;

    struct stream_ctx ctx = {.callbacks = callbacks,
                             .choice_index = choice_index,
                             .accums = NULL,
                             .accums_count = 0,
                             .max_tool_args = client->limits.max_tool_args_bytes_per_call};
    sse_parser_t* sse = sse_create(client->limits.max_line_bytes, client->limits.max_frame_bytes,
                                   client->limits.max_sse_buffer_bytes, client->limits.max_response_bytes);
    sse_set_callback(sse, on_sse_data, &ctx);

    curl_stream_ctx cs = {sse, &ctx, SSE_OK};
    struct header_set header_set;
    if (!llm_header_set_init(&header_set, client, headers, headers_count)) {
        sse_destroy(sse);
        free(request_json);
        return false;
    }
    llm_tls_config_t tls;
    const llm_tls_config_t* tls_ptr = llm_client_tls_config(client, &tls);
    bool ok = http_post_stream(url, request_json, client->timeout.overall_timeout_ms,
                               client->timeout.read_idle_timeout_ms, header_set.headers, header_set.count, tls_ptr,
                               client->proxy_url, client->no_proxy, curl_stream_cb, &cs);
    header_set_free(&header_set);
    if (cs.sse_error != SSE_OK) {
        ok = false;
    }

    for (size_t i = 0; i < ctx.accums_count; i++) {
        accum_free(&ctx.accums[i]);
    }
    free(ctx.accums);
    sse_destroy(sse);
    free(request_json);

    return ok;
}

bool llm_chat_stream_with_headers(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                  const char* params_json, const char* tooling_json, const char* response_format_json,
                                  const llm_stream_callbacks_t* callbacks, const char* const* headers,
                                  size_t headers_count) {
    return llm_chat_stream_with_headers_choice(client, messages, messages_count, params_json, tooling_json,
                                               response_format_json, 0, callbacks, headers, headers_count);
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
    return llm_chat_stream_with_headers_choice(client, messages, messages_count, params_json, tooling_json,
                                               response_format_json, choice_index, callbacks, NULL, 0);
}

bool llm_chat_stream_choice_with_headers(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                         const char* params_json, const char* tooling_json,
                                         const char* response_format_json, size_t choice_index,
                                         const llm_stream_callbacks_t* callbacks, const char* const* headers,
                                         size_t headers_count) {
    return llm_chat_stream_with_headers_choice(client, messages, messages_count, params_json, tooling_json,
                                               response_format_json, choice_index, callbacks, headers, headers_count);
}

// Tool loop implementation is usually complex, let's put a simplified version here
bool llm_tool_loop_run_with_headers(llm_client_t* client, const llm_message_t* initial_messages, size_t initial_count,
                                    const char* tooling_json, llm_tool_dispatch_cb dispatch, void* dispatch_user_data,
                                    size_t max_turns, const char* const* headers, size_t headers_count) {
    size_t history_count = initial_count;
    llm_message_t* history = calloc(history_count, sizeof(llm_message_t));
    if (!history) {
        return false;
    }

    for (size_t i = 0; i < initial_count; i++) {
        history[i].role = initial_messages[i].role;
        if (initial_messages[i].content) {
            history[i].content = strdup(initial_messages[i].content);
            if (!history[i].content) {
                // Free already duplicated content and history itself
                for (size_t j = 0; j < i; j++) {
                    llm_message_free_content(&history[j]);
                }
                free(history);
                return false;
            }
            history[i].content_len = initial_messages[i].content_len;
        }
        if (initial_messages[i].tool_call_id) {
            history[i].tool_call_id = strdup(initial_messages[i].tool_call_id);
            if (!history[i].tool_call_id) {
                llm_message_free_content(&history[i]);  // Free current content
                for (size_t j = 0; j < i; j++) {
                    llm_message_free_content(&history[j]);
                }
                free(history);
                return false;
            }
            history[i].tool_call_id_len = initial_messages[i].tool_call_id_len;
        }
        history[i].name =
            initial_messages[i].name;  // name is not duplicated as it's typically static or managed elsewhere
        history[i].name_len = initial_messages[i].name_len;
    }

    bool success = true;
    for (size_t turn = 0; turn < max_turns; turn++) {
        llm_chat_result_t result;
        if (!llm_chat_with_headers(client, history, history_count, NULL, tooling_json, NULL, &result, headers,
                                   headers_count)) {
            success = false;
            break;
        }

        if (result.finish_reason != LLM_FINISH_REASON_TOOL_CALLS) {
            llm_chat_result_free(&result);
            break;
        }

        // Prepare for new messages (assistant + tool results)
        size_t next_history_idx = history_count;
        size_t new_total_count = history_count + 1 + result.tool_calls_count;
        llm_message_t* new_history = realloc(history, new_total_count * sizeof(llm_message_t));
        if (!new_history) {
            // Realloc failed, free all current history and the chat result
            for (size_t i = 0; i < history_count; i++) {
                llm_message_free_content(&history[i]);
            }
            free(history);
            llm_chat_result_free(&result);
            success = false;
            break;
        }
        history = new_history;

        // Assistant message
        llm_message_t* assistant_msg = &history[next_history_idx++];
        memset(assistant_msg, 0, sizeof(*assistant_msg));
        assistant_msg->role = LLM_ROLE_ASSISTANT;

        // Combine content and reasoning_content if available
        if (result.content || result.reasoning_content) {
            size_t total_len = 0;
            if (result.content) total_len += result.content_len;
            if (result.reasoning_content) total_len += result.reasoning_content_len;

            char* combined_content = malloc(total_len + 1);  // +1 for null terminator
            if (!combined_content) {
                // Handle allocation failure, clean up
                for (size_t i = 0; i < next_history_idx - 1; i++) {
                    llm_message_free_content(&history[i]);
                }
                free(history);
                llm_chat_result_free(&result);
                success = false;
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
        for (size_t i = 0; i < result.tool_calls_count; i++) {
            char* res_json = NULL;
            size_t res_len = 0;

            // Call dispatch function
            if (!dispatch(dispatch_user_data, result.tool_calls[i].name, result.tool_calls[i].name_len,
                          result.tool_calls[i].arguments, result.tool_calls[i].arguments_len, &res_json, &res_len)) {
                // Dispatch failed. Clean up all history messages and chat result.
                if (res_json) free(res_json);  // Free res_json if allocated by dispatch before failure
                for (size_t j = 0; j < history_count; j++) {
                    llm_message_free_content(&history[j]);
                }
                free(history);
                llm_chat_result_free(&result);
                success = false;
                goto cleanup_loop;  // Exit tool loop
            }

            // If dispatch succeeded but returned no res_json, treat as failure for loop purposes
            if (!res_json) {
                for (size_t j = 0; j < history_count; j++) {
                    llm_message_free_content(&history[j]);
                }
                free(history);
                llm_chat_result_free(&result);
                success = false;
                goto cleanup_loop;  // Exit tool loop
            }

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
                    for (size_t j = 0; j < next_history_idx - 1; j++) {
                        llm_message_free_content(&history[j]);
                    }
                    free(history);
                    llm_chat_result_free(&result);
                    success = false;
                    goto cleanup_loop;  // Exit tool loop
                }
                tool_msg->tool_call_id_len = result.tool_calls[i].id_len;
            }
            history_count++;  // Increment history count for each tool message
        }

        llm_chat_result_free(&result);
    }

cleanup_loop:
    // Free history copies if we made them
    for (size_t i = 0; i < history_count; i++) {
        llm_message_free_content(&history[i]);
    }
    free(history);
    return success;
}

bool llm_tool_loop_run(llm_client_t* client, const llm_message_t* initial_messages, size_t initial_count,
                       const char* tooling_json, llm_tool_dispatch_cb dispatch, void* dispatch_user_data,
                       size_t max_turns) {
    return llm_tool_loop_run_with_headers(client, initial_messages, initial_count, tooling_json, dispatch,
                                          dispatch_user_data, max_turns, NULL, 0);
}
