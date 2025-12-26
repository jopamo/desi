#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSTOK_HEADER
#include <jstok.h>

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
    const char* stream_payload;
    size_t stream_payload_len;
    size_t stream_chunk_size;
    char* request_body;
    size_t request_len;
    bool called_stream;
};

static struct fake_transport_state g_fake;

static void fake_reset(void) {
    free(g_fake.request_body);
    memset(&g_fake, 0, sizeof(g_fake));
}

static bool capture_request(const char* json_body) {
    if (!json_body) return false;
    size_t len = strlen(json_body);
    char* copy = malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, json_body, len);
    copy[len] = '\0';
    free(g_fake.request_body);
    g_fake.request_body = copy;
    g_fake.request_len = len;
    return true;
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
    (void)json_body;
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

bool http_post_stream(const char* url, const char* json_body, long timeout_ms, long read_idle_timeout_ms,
                      const char* const* headers, size_t headers_count, const llm_tls_config_t* tls,
                      const char* proxy_url, const char* no_proxy, stream_cb cb, void* user_data) {
    (void)url;
    (void)timeout_ms;
    (void)read_idle_timeout_ms;
    (void)headers;
    (void)headers_count;
    (void)tls;
    (void)proxy_url;
    (void)no_proxy;

    g_fake.called_stream = true;
    if (!capture_request(json_body)) return false;
    if (!g_fake.stream_payload || g_fake.stream_payload_len == 0) return false;

    size_t chunk_size = g_fake.stream_chunk_size ? g_fake.stream_chunk_size : g_fake.stream_payload_len;
    size_t offset = 0;
    while (offset < g_fake.stream_payload_len) {
        size_t remaining = g_fake.stream_payload_len - offset;
        size_t take = remaining < chunk_size ? remaining : chunk_size;
        if (!cb(g_fake.stream_payload + offset, take, user_data)) return false;
        offset += take;
    }
    return true;
}

struct usage_capture {
    size_t calls;
    llm_usage_t last;
};

static void on_usage(void* user_data, const llm_usage_t* usage) {
    struct usage_capture* cap = user_data;
    cap->calls++;
    cap->last = *usage;
}

static bool parse_request_include_usage(const char* json, size_t len, bool* present, bool* value) {
    if (!present || !value || !json) return false;
    *present = false;
    *value = false;

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

    int stream_options_idx = jstok_object_get(json, tokens, parsed, 0, "stream_options");
    if (stream_options_idx < 0) {
        free(tokens);
        return true;
    }
    if (tokens[stream_options_idx].type != JSTOK_OBJECT) {
        free(tokens);
        return false;
    }
    int include_idx = jstok_object_get(json, tokens, parsed, stream_options_idx, "include_usage");
    if (include_idx < 0) {
        free(tokens);
        return true;
    }
    if (tokens[include_idx].type != JSTOK_PRIMITIVE) {
        free(tokens);
        return false;
    }
    int bool_val = 0;
    if (jstok_atob(json, &tokens[include_idx], &bool_val) != 0) {
        free(tokens);
        return false;
    }

    *present = true;
    *value = bool_val != 0;
    free(tokens);
    return true;
}

static bool test_chat_usage_midstream(void) {
    fake_reset();
    static const char stream_sse[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
        "data: "
        "{\"choices\":[{\"delta\":{\"content\":\"!\"}}],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4,\"total_"
        "tokens\":7}}\n\n"
        "data: [DONE]\n\n";

    g_fake.stream_payload = stream_sse;
    g_fake.stream_payload_len = strlen(stream_sse);
    g_fake.stream_chunk_size = 7;

    llm_model_t model = {.name = "fake-model"};
    llm_client_t* client = llm_client_create("http://fake", &model, NULL, NULL);
    if (!require(client != NULL, "client create failed")) return false;

    llm_message_t msg = {LLM_ROLE_USER, "hi", 2, NULL, 0, NULL, 0, NULL, 0};
    struct usage_capture cap = {0};
    llm_stream_callbacks_t cbs = {0};
    cbs.user_data = &cap;
    cbs.on_usage = on_usage;
    cbs.include_usage = true;

    bool ok = llm_chat_stream(client, &msg, 1, NULL, NULL, NULL, &cbs);
    bool present = false;
    bool value = false;
    bool parsed = parse_request_include_usage(g_fake.request_body, g_fake.request_len, &present, &value);

    llm_client_destroy(client);

    if (!require(ok, "chat stream failed")) return false;
    if (!require(g_fake.called_stream, "stream not called")) return false;
    if (!require(parsed, "parse request include_usage failed")) return false;
    if (!require(present && value, "include_usage not set")) return false;
    if (!require(cap.calls == 1, "usage callback count")) return false;
    if (!require(cap.last.has_prompt_tokens && cap.last.prompt_tokens == 3, "usage prompt tokens")) return false;
    if (!require(cap.last.has_completion_tokens && cap.last.completion_tokens == 4, "usage completion tokens")) {
        return false;
    }
    if (!require(cap.last.has_total_tokens && cap.last.total_tokens == 7, "usage total tokens")) return false;

    return true;
}

static bool test_completions_usage_completion_only(void) {
    fake_reset();
    static const char stream_sse[] =
        "data: {\"choices\":[{\"text\":\"hi\"}]}\n\n"
        "data: "
        "{\"choices\":[{\"text\":\"!\",\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":2,\"completion_"
        "tokens\":1,\"total_tokens\":3}}\n\n"
        "data: [DONE]\n\n";

    g_fake.stream_payload = stream_sse;
    g_fake.stream_payload_len = strlen(stream_sse);
    g_fake.stream_chunk_size = 5;

    llm_model_t model = {.name = "fake-model"};
    llm_client_t* client = llm_client_create("http://fake", &model, NULL, NULL);
    if (!require(client != NULL, "client create failed")) return false;

    struct usage_capture cap = {0};
    llm_stream_callbacks_t cbs = {0};
    cbs.user_data = &cap;
    cbs.on_usage = on_usage;
    cbs.include_usage = true;

    bool ok = llm_completions_stream(client, "prompt", 6, NULL, &cbs);
    bool present = false;
    bool value = false;
    bool parsed = parse_request_include_usage(g_fake.request_body, g_fake.request_len, &present, &value);

    llm_client_destroy(client);

    if (!require(ok, "completions stream failed")) return false;
    if (!require(g_fake.called_stream, "stream not called")) return false;
    if (!require(parsed, "parse request include_usage failed")) return false;
    if (!require(present && value, "include_usage not set")) return false;
    if (!require(cap.calls == 1, "usage callback count")) return false;
    if (!require(cap.last.has_prompt_tokens && cap.last.prompt_tokens == 2, "usage prompt tokens")) return false;
    if (!require(cap.last.has_completion_tokens && cap.last.completion_tokens == 1, "usage completion tokens")) {
        return false;
    }
    if (!require(cap.last.has_total_tokens && cap.last.total_tokens == 3, "usage total tokens")) return false;

    return true;
}

static bool test_chat_usage_omitted(void) {
    fake_reset();
    static const char stream_sse[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";

    g_fake.stream_payload = stream_sse;
    g_fake.stream_payload_len = strlen(stream_sse);
    g_fake.stream_chunk_size = 11;

    llm_model_t model = {.name = "fake-model"};
    llm_client_t* client = llm_client_create("http://fake", &model, NULL, NULL);
    if (!require(client != NULL, "client create failed")) return false;

    llm_message_t msg = {LLM_ROLE_USER, "hi", 2, NULL, 0, NULL, 0, NULL, 0};
    struct usage_capture cap = {0};
    llm_stream_callbacks_t cbs = {0};
    cbs.user_data = &cap;
    cbs.on_usage = on_usage;
    cbs.include_usage = true;

    bool ok = llm_chat_stream(client, &msg, 1, NULL, NULL, NULL, &cbs);
    bool present = false;
    bool value = false;
    bool parsed = parse_request_include_usage(g_fake.request_body, g_fake.request_len, &present, &value);

    llm_client_destroy(client);

    if (!require(ok, "chat stream failed")) return false;
    if (!require(g_fake.called_stream, "stream not called")) return false;
    if (!require(parsed, "parse request include_usage failed")) return false;
    if (!require(present && value, "include_usage not set")) return false;
    if (!require(cap.calls == 0, "usage callback should not fire")) return false;

    return true;
}

int main(void) {
    if (!test_chat_usage_midstream()) return 1;
    if (!test_completions_usage_completion_only()) return 1;
    if (!test_chat_usage_omitted()) return 1;
    printf("Streaming usage tests passed.\n");
    return 0;
}
