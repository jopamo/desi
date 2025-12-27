#include "fake_transport.h"

#include <stdlib.h>
#include <string.h>

static fake_transport_state_t g_state;
static char* g_stream_scratch;
static size_t g_stream_scratch_cap;

static bool header_list_contains(const char* const* headers, size_t headers_count, const char* expected) {
    if (!headers || !expected) return false;
    for (size_t i = 0; i < headers_count; i++) {
        if (headers[i] && strcmp(headers[i], expected) == 0) return true;
    }
    return false;
}

static bool check_headers(const fake_transport_state_t* state, const char* const* headers, size_t headers_count) {
    if (!state->expected_headers || state->expected_headers_count == 0) return true;
    if (!headers || headers_count < state->expected_headers_count) return false;
    for (size_t i = 0; i < state->expected_headers_count; i++) {
        if (!header_list_contains(headers, headers_count, state->expected_headers[i])) return false;
    }
    return true;
}

static bool check_proxy(const fake_transport_state_t* state, const char* proxy_url, const char* no_proxy) {
    if (state->expected_proxy_url) {
        if (!proxy_url || strcmp(proxy_url, state->expected_proxy_url) != 0) return false;
    }
    if (state->expected_no_proxy) {
        if (!no_proxy || strcmp(no_proxy, state->expected_no_proxy) != 0) return false;
    }
    return true;
}

static size_t resolve_len(const char* data, size_t explicit_len) {
    if (!data) return 0;
    if (explicit_len > 0) return explicit_len;
    return strlen(data);
}

static size_t resolve_post_count(const fake_transport_state_t* state) {
    if (state->post_responses_count > 0) return state->post_responses_count;
    size_t count = 0;
    while (count < FAKE_TRANSPORT_MAX_POST_RESPONSES && state->post_responses[count]) {
        count++;
    }
    return count;
}

static bool ensure_stream_scratch(size_t len) {
    if (len == 0) return true;
    if (g_stream_scratch_cap >= len) return true;
    size_t new_cap = g_stream_scratch_cap ? g_stream_scratch_cap : 64;
    if (new_cap < len) {
        new_cap = len;
    }
    char* next = realloc(g_stream_scratch, new_cap);
    if (!next) return false;
    g_stream_scratch = next;
    g_stream_scratch_cap = new_cap;
    return true;
}

static bool capture_request(fake_transport_state_t* state, const char* json_body) {
    state->last_request_body = NULL;
    state->last_request_len = 0;
    if (!json_body) return true;
    if (state->request_count >= FAKE_TRANSPORT_MAX_REQUESTS) return false;

    size_t len = strlen(json_body);
    char* copy = malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, json_body, len);
    copy[len] = '\0';

    state->request_bodies[state->request_count] = copy;
    state->request_lens[state->request_count] = len;
    state->request_count++;
    state->last_request_body = copy;
    state->last_request_len = len;
    return true;
}

static void transport_status_init(llm_transport_status_t* status, long http_status) {
    if (!status) return;
    status->http_status = http_status;
    status->curl_code = 0;
    status->tls_error = false;
}

fake_transport_state_t* fake_transport_state(void) { return &g_state; }

void fake_transport_reset(void) {
    for (size_t i = 0; i < g_state.request_count; i++) {
        free(g_state.request_bodies[i]);
        g_state.request_bodies[i] = NULL;
        g_state.request_lens[i] = 0;
    }
    g_state.request_count = 0;
    g_state.last_request_body = NULL;
    g_state.last_request_len = 0;

    memset(&g_state, 0, sizeof(g_state));
    g_state.status_get = 200;
    g_state.status_post = 200;
    g_state.status_stream = 200;
    g_state.headers_ok = true;
    g_state.proxy_ok = true;
    g_state.stream_use_scratch = true;
    g_state.stream_scratch_fill = 'x';

    free(g_stream_scratch);
    g_stream_scratch = NULL;
    g_stream_scratch_cap = 0;
}

bool http_get(const char* url, long timeout_ms, size_t max_response_bytes, const char* const* headers,
              size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url, const char* no_proxy,
              char** body, size_t* len, llm_transport_status_t* status) {
    (void)timeout_ms;
    (void)tls;

    transport_status_init(status, g_state.status_get);
    g_state.called_get = true;
    g_state.get_calls++;
    g_state.headers_ok = g_state.headers_ok && check_headers(&g_state, headers, headers_count);
    g_state.proxy_ok = g_state.proxy_ok && check_proxy(&g_state, proxy_url, no_proxy);
    if (g_state.expected_url && (!url || strcmp(url, g_state.expected_url) != 0)) {
        g_state.headers_ok = false;
    }

    if (g_state.fail_get || !g_state.response_get) {
        if (body) *body = NULL;
        if (len) *len = 0;
        transport_status_init(status, 0);
        return false;
    }

    size_t resp_len = resolve_len(g_state.response_get, g_state.response_get_len);
    if (max_response_bytes > 0 && resp_len > max_response_bytes) {
        if (body) *body = NULL;
        if (len) *len = 0;
        transport_status_init(status, 0);
        return false;
    }

    char* resp = malloc(resp_len + 1);
    if (!resp) return false;
    if (resp_len > 0) {
        memcpy(resp, g_state.response_get, resp_len);
    }
    resp[resp_len] = '\0';

    g_state.last_body = resp;
    g_state.last_body_len = resp_len;
    if (body) *body = resp;
    if (len) *len = resp_len;
    return true;
}

bool http_post(const char* url, const char* json_body, long timeout_ms, size_t max_response_bytes,
               const char* const* headers, size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url,
               const char* no_proxy, char** body, size_t* len, llm_transport_status_t* status) {
    (void)timeout_ms;
    (void)tls;

    transport_status_init(status, g_state.status_post);
    g_state.called_post = true;
    g_state.headers_ok = g_state.headers_ok && check_headers(&g_state, headers, headers_count);
    g_state.proxy_ok = g_state.proxy_ok && check_proxy(&g_state, proxy_url, no_proxy);
    if (g_state.expected_url && (!url || strcmp(url, g_state.expected_url) != 0)) {
        g_state.headers_ok = false;
    }

    size_t index = g_state.post_calls;
    size_t count = resolve_post_count(&g_state);
    if (count > 0 && index >= count) {
        if (body) *body = NULL;
        if (len) *len = 0;
        transport_status_init(status, 0);
        return false;
    }

    if (!capture_request(&g_state, json_body)) {
        if (body) *body = NULL;
        if (len) *len = 0;
        transport_status_init(status, 0);
        return false;
    }

    const char* response = g_state.response_post;
    size_t response_len = g_state.response_post_len;
    if (count > 0) {
        response = g_state.post_responses[index];
        response_len = g_state.post_response_lens[index];
        g_state.post_calls++;
    }

    if (g_state.fail_post || !response) {
        if (body) *body = NULL;
        if (len) *len = 0;
        transport_status_init(status, 0);
        return false;
    }

    size_t resp_len = resolve_len(response, response_len);
    if (max_response_bytes > 0 && resp_len > max_response_bytes) {
        if (body) *body = NULL;
        if (len) *len = 0;
        transport_status_init(status, 0);
        return false;
    }

    char* resp = malloc(resp_len + 1);
    if (!resp) return false;
    if (resp_len > 0) {
        memcpy(resp, response, resp_len);
    }
    resp[resp_len] = '\0';

    g_state.last_body = resp;
    g_state.last_body_len = resp_len;
    if (body) *body = resp;
    if (len) *len = resp_len;
    return true;
}

static bool stream_emit_chunk(const char* data, size_t len, stream_cb cb, void* user_data) {
    if (!cb) return false;
    const char* ptr = data;
    if (g_state.stream_use_scratch) {
        if (!ensure_stream_scratch(len)) return false;
        if (len > 0) {
            memcpy(g_stream_scratch, data, len);
        }
        ptr = g_stream_scratch;
    }

    bool keep = cb(ptr, len, user_data);
    g_state.stream_cb_calls++;

    if (g_state.stream_use_scratch && len > 0) {
        memset(g_stream_scratch, g_state.stream_scratch_fill, len);
    }
    return keep;
}

bool http_post_stream(const char* url, const char* json_body, long timeout_ms, long read_idle_timeout_ms,
                      const char* const* headers, size_t headers_count, const llm_tls_config_t* tls,
                      const char* proxy_url, const char* no_proxy, stream_cb cb, void* user_data,
                      llm_transport_status_t* status) {
    (void)timeout_ms;
    (void)read_idle_timeout_ms;
    (void)tls;

    transport_status_init(status, g_state.status_stream);
    g_state.called_stream = true;
    g_state.stream_calls++;
    g_state.headers_ok = g_state.headers_ok && check_headers(&g_state, headers, headers_count);
    g_state.proxy_ok = g_state.proxy_ok && check_proxy(&g_state, proxy_url, no_proxy);
    if (g_state.expected_url && (!url || strcmp(url, g_state.expected_url) != 0)) {
        g_state.headers_ok = false;
    }

    if (!capture_request(&g_state, json_body)) {
        transport_status_init(status, 0);
        return false;
    }

    if (g_state.fail_stream) {
        transport_status_init(status, 0);
        return false;
    }

    if (g_state.stream_chunks_count > 0) {
        if (!g_state.stream_chunks) {
            transport_status_init(status, 0);
            return false;
        }
        for (size_t i = 0; i < g_state.stream_chunks_count; i++) {
            const fake_stream_chunk_t* chunk = &g_state.stream_chunks[i];
            g_state.headers_ok = g_state.headers_ok && check_headers(&g_state, headers, headers_count);
            bool keep = stream_emit_chunk(chunk->data, chunk->len, cb, user_data);
            g_state.headers_ok = g_state.headers_ok && check_headers(&g_state, headers, headers_count);
            if (!keep) return false;
        }
        return true;
    }

    if (!g_state.stream_payload || g_state.stream_payload_len == 0) {
        transport_status_init(status, 0);
        return false;
    }

    size_t chunk_size = g_state.stream_chunk_size ? g_state.stream_chunk_size : g_state.stream_payload_len;
    size_t offset = 0;
    while (offset < g_state.stream_payload_len) {
        size_t remaining = g_state.stream_payload_len - offset;
        size_t take = remaining < chunk_size ? remaining : chunk_size;
        g_state.headers_ok = g_state.headers_ok && check_headers(&g_state, headers, headers_count);
        bool keep = stream_emit_chunk(g_state.stream_payload + offset, take, cb, user_data);
        g_state.headers_ok = g_state.headers_ok && check_headers(&g_state, headers, headers_count);
        if (!keep) return false;
        offset += take;
    }
    return true;
}
