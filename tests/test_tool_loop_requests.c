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

struct dispatch_state {
    size_t calls;
    const char* reply;
    size_t reply_len;
};

static void fake_reset(void) {
    for (size_t i = 0; i < (sizeof(g_fake.request_bodies) / sizeof(g_fake.request_bodies[0])); i++) {
        free(g_fake.request_bodies[i]);
    }
    memset(&g_fake, 0, sizeof(g_fake));
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
    (void)cb;
    (void)user_data;
    if (status) {
        status->http_status = 0;
        status->curl_code = 0;
        status->tls_error = false;
    }
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

static bool extract_request_controls(const char* json, size_t len, span_t* temperature, span_t* response_type,
                                     span_t* tool_choice_name) {
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

    int temp_idx = obj_get_key(tokens, parsed, 0, json, "temperature");
    if (temp_idx < 0 || tokens[temp_idx].type != JSTOK_PRIMITIVE) goto cleanup;
    int resp_idx = obj_get_key(tokens, parsed, 0, json, "response_format");
    if (resp_idx < 0 || tokens[resp_idx].type != JSTOK_OBJECT) goto cleanup;
    int type_idx = obj_get_key(tokens, parsed, resp_idx, json, "type");
    if (type_idx < 0 || tokens[type_idx].type != JSTOK_STRING) goto cleanup;
    int tool_choice_idx = obj_get_key(tokens, parsed, 0, json, "tool_choice");
    if (tool_choice_idx < 0 || tokens[tool_choice_idx].type != JSTOK_OBJECT) goto cleanup;
    int func_idx = obj_get_key(tokens, parsed, tool_choice_idx, json, "function");
    if (func_idx < 0 || tokens[func_idx].type != JSTOK_OBJECT) goto cleanup;
    int name_idx = obj_get_key(tokens, parsed, func_idx, json, "name");
    if (name_idx < 0 || tokens[name_idx].type != JSTOK_STRING) goto cleanup;

    *temperature = tok_span(json, &tokens[temp_idx]);
    *response_type = tok_span(json, &tokens[type_idx]);
    *tool_choice_name = tok_span(json, &tokens[name_idx]);
    ok = true;

cleanup:
    free(tokens);
    return ok;
}

static bool extract_first_message(const char* json, size_t len, span_t* role, span_t* content) {
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
    if (messages_idx < 0 || tokens[messages_idx].type != JSTOK_ARRAY || tokens[messages_idx].size <= 0) {
        goto cleanup;
    }
    int msg_idx = arr_get(tokens, parsed, messages_idx, 0);
    if (msg_idx < 0 || tokens[msg_idx].type != JSTOK_OBJECT) goto cleanup;
    int role_idx = obj_get_key(tokens, parsed, msg_idx, json, "role");
    if (role_idx < 0 || tokens[role_idx].type != JSTOK_STRING) goto cleanup;
    int content_idx = obj_get_key(tokens, parsed, msg_idx, json, "content");
    if (content_idx < 0 || tokens[content_idx].type != JSTOK_STRING) goto cleanup;
    *role = tok_span(json, &tokens[role_idx]);
    *content = tok_span(json, &tokens[content_idx]);
    ok = true;

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

static bool dispatch_fixed_reply(void* user_data, const char* tool_name, size_t name_len, const char* args_json,
                                 size_t args_len, char** result_json, size_t* result_len) {
    struct dispatch_state* state = user_data;
    (void)tool_name;
    (void)name_len;
    (void)args_json;
    (void)args_len;
    if (!state || !state->reply) return false;
    state->calls++;
    char* out = malloc(state->reply_len + 1);
    if (!out) return false;
    memcpy(out, state->reply, state->reply_len);
    out[state->reply_len] = '\0';
    *result_json = out;
    *result_len = state->reply_len;
    return true;
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
    llm_message_t msg = {LLM_ROLE_USER, "run tool", 8, NULL, 0, NULL, 0, NULL, 0, NULL, 0};

    bool ok = llm_tool_loop_run(client, &msg, 1, NULL, tooling_json, NULL, tool_dispatch, NULL, 3);
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

static bool test_tool_loop_params_passthrough(void) {
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

    const char* params_json = "{\"temperature\":0.2,\"seed\":44}";
    const char* tooling_json =
        "{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"add\"}}],"
        "\"tool_choice\":{\"type\":\"function\",\"function\":{\"name\":\"add\"}}}";
    const char* response_format_json = "{\"type\":\"json_object\"}";
    llm_message_t msg = {LLM_ROLE_USER, "run tool", 8, NULL, 0, NULL, 0, NULL, 0, NULL, 0};

    bool ok =
        llm_tool_loop_run(client, &msg, 1, params_json, tooling_json, response_format_json, tool_dispatch, NULL, 3);
    assert_true(ok, "tool loop failed");
    assert_true(g_fake.post_calls == 2, "expected two post calls");

    for (size_t i = 0; i < g_fake.post_calls; i++) {
        span_t temperature = {0};
        span_t response_type = {0};
        span_t tool_choice_name = {0};
        assert_true(extract_request_controls(g_fake.request_bodies[i], g_fake.request_lens[i], &temperature,
                                             &response_type, &tool_choice_name),
                    "request controls missing");
        assert_true(temperature.len == 3 && memcmp(temperature.ptr, "0.2", 3) == 0, "temperature mismatch");
        assert_true(response_type.len == 11 && memcmp(response_type.ptr, "json_object", 11) == 0,
                    "response_format mismatch");
        assert_true(tool_choice_name.len == 3 && memcmp(tool_choice_name.ptr, "add", 3) == 0,
                    "tool_choice name mismatch");
    }

    llm_client_destroy(client);
    return true;
}

static bool test_tool_loop_detects_repeat(void) {
    fake_reset();
    g_fake.post_responses[0] =
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
        "\"name\":\"add\",\"arguments\":\"42\"}}]},\"finish_reason\":\"tool_calls\"}]}";
    g_fake.post_responses[1] =
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_2\",\"type\":\"function\",\"function\":{"
        "\"name\":\"add\",\"arguments\":\"42\"}}]},\"finish_reason\":\"tool_calls\"}]}";

    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {1000, 2000, 2000};
    llm_limits_t limits = {0};
    limits.max_response_bytes = 64 * 1024;
    limits.max_line_bytes = 1024;
    limits.max_frame_bytes = 1024;
    limits.max_sse_buffer_bytes = 64 * 1024;
    limits.max_tool_args_bytes_per_call = 1024;
    limits.max_tool_args_bytes_per_turn = 1024;
    limits.max_tool_output_bytes_total = 1024;
    llm_client_t* client = llm_client_create("http://fake", &model, &timeout, &limits);
    assert_true(client != NULL, "client create failed");

    const char* tooling_json = "{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"add\"}}]}";
    llm_message_t msg = {LLM_ROLE_USER, "run tool", 8, NULL, 0, NULL, 0, NULL, 0, NULL, 0};

    struct dispatch_state state = {0, "43", 2};
    bool ok = llm_tool_loop_run(client, &msg, 1, NULL, tooling_json, NULL, dispatch_fixed_reply, &state, 4);
    assert_true(!ok, "expected loop detection failure");
    assert_true(state.calls == 1, "expected single dispatch");
    assert_true(g_fake.post_calls == 2, "expected two post calls");

    span_t role = {0};
    span_t content = {0};
    assert_true(extract_first_message(g_fake.request_bodies[0], g_fake.request_lens[0], &role, &content),
                "first request message parse failed");
    assert_true(role.len == 4 && memcmp(role.ptr, "user", 4) == 0, "role mismatch");
    assert_true(content.len == 8 && memcmp(content.ptr, "run tool", 8) == 0, "content mismatch");

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

static bool test_tool_loop_max_turns(void) {
    fake_reset();
    g_fake.post_responses[0] =
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
        "\"name\":\"add\",\"arguments\":\"42\"}}]},\"finish_reason\":\"tool_calls\"}]}";

    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {1000, 2000, 2000};
    llm_limits_t limits = {0};
    limits.max_response_bytes = 64 * 1024;
    limits.max_line_bytes = 1024;
    limits.max_frame_bytes = 1024;
    limits.max_sse_buffer_bytes = 64 * 1024;
    limits.max_tool_args_bytes_per_call = 1024;
    limits.max_tool_args_bytes_per_turn = 1024;
    limits.max_tool_output_bytes_total = 1024;
    llm_client_t* client = llm_client_create("http://fake", &model, &timeout, &limits);
    assert_true(client != NULL, "client create failed");

    const char* tooling_json = "{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"add\"}}]}";
    llm_message_t msg = {LLM_ROLE_USER, "run tool", 8, NULL, 0, NULL, 0, NULL, 0, NULL, 0};

    struct dispatch_state state = {0, "43", 2};
    bool ok = llm_tool_loop_run(client, &msg, 1, NULL, tooling_json, NULL, dispatch_fixed_reply, &state, 1);
    assert_true(!ok, "expected max turns failure");
    assert_true(state.calls == 0, "dispatch should not run");
    assert_true(g_fake.post_calls == 1, "expected one post call");

    span_t role = {0};
    span_t content = {0};
    assert_true(extract_first_message(g_fake.request_bodies[0], g_fake.request_lens[0], &role, &content),
                "first request message parse failed");
    assert_true(role.len == 4 && memcmp(role.ptr, "user", 4) == 0, "role mismatch");
    assert_true(content.len == 8 && memcmp(content.ptr, "run tool", 8) == 0, "content mismatch");

    llm_client_destroy(client);
    return true;
}

static bool test_tool_loop_args_limit(void) {
    fake_reset();
    g_fake.post_responses[0] =
        "{\"choices\":[{\"message\":{\"tool_calls\":["
        "{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"add\",\"arguments\":\"123\"}},"
        "{\"id\":\"call_2\",\"type\":\"function\",\"function\":{\"name\":\"sub\",\"arguments\":\"456\"}}"
        "]},\"finish_reason\":\"tool_calls\"}]}";

    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {1000, 2000, 2000};
    llm_limits_t limits = {0};
    limits.max_response_bytes = 64 * 1024;
    limits.max_line_bytes = 1024;
    limits.max_frame_bytes = 1024;
    limits.max_sse_buffer_bytes = 64 * 1024;
    limits.max_tool_args_bytes_per_call = 1024;
    limits.max_tool_args_bytes_per_turn = 4;
    limits.max_tool_output_bytes_total = 0;
    llm_client_t* client = llm_client_create("http://fake", &model, &timeout, &limits);
    assert_true(client != NULL, "client create failed");

    const char* tooling_json =
        "{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"add\"}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"sub\"}}]}";
    llm_message_t msg = {LLM_ROLE_USER, "run tool", 8, NULL, 0, NULL, 0, NULL, 0, NULL, 0};

    struct dispatch_state state = {0, "43", 2};
    bool ok = llm_tool_loop_run(client, &msg, 1, NULL, tooling_json, NULL, dispatch_fixed_reply, &state, 3);
    assert_true(!ok, "expected tool args limit failure");
    assert_true(state.calls == 0, "dispatch should not run");
    assert_true(g_fake.post_calls == 1, "expected one post call");

    span_t role = {0};
    span_t content = {0};
    assert_true(extract_first_message(g_fake.request_bodies[0], g_fake.request_lens[0], &role, &content),
                "first request message parse failed");
    assert_true(role.len == 4 && memcmp(role.ptr, "user", 4) == 0, "role mismatch");
    assert_true(content.len == 8 && memcmp(content.ptr, "run tool", 8) == 0, "content mismatch");

    llm_client_destroy(client);
    return true;
}

static bool test_tool_loop_output_limit(void) {
    fake_reset();
    g_fake.post_responses[0] =
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
        "\"name\":\"add\",\"arguments\":\"1\"}}]},\"finish_reason\":\"tool_calls\"}]}";
    g_fake.post_responses[1] =
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_2\",\"type\":\"function\",\"function\":{"
        "\"name\":\"sub\",\"arguments\":\"2\"}}]},\"finish_reason\":\"tool_calls\"}]}";

    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {1000, 2000, 2000};
    llm_limits_t limits = {0};
    limits.max_response_bytes = 64 * 1024;
    limits.max_line_bytes = 1024;
    limits.max_frame_bytes = 1024;
    limits.max_sse_buffer_bytes = 64 * 1024;
    limits.max_tool_args_bytes_per_call = 1024;
    limits.max_tool_args_bytes_per_turn = 1024;
    limits.max_tool_output_bytes_total = 4;
    llm_client_t* client = llm_client_create("http://fake", &model, &timeout, &limits);
    assert_true(client != NULL, "client create failed");

    const char* tooling_json =
        "{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"add\"}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"sub\"}}]}";
    llm_message_t msg = {LLM_ROLE_USER, "run tool", 8, NULL, 0, NULL, 0, NULL, 0, NULL, 0};

    struct dispatch_state state = {0, "abc", 3};
    bool ok = llm_tool_loop_run(client, &msg, 1, NULL, tooling_json, NULL, dispatch_fixed_reply, &state, 3);
    assert_true(!ok, "expected tool output limit failure");
    assert_true(state.calls == 2, "expected two dispatch calls");
    assert_true(g_fake.post_calls == 2, "expected two post calls");

    span_t role = {0};
    span_t content = {0};
    assert_true(extract_first_message(g_fake.request_bodies[0], g_fake.request_lens[0], &role, &content),
                "first request message parse failed");
    assert_true(role.len == 4 && memcmp(role.ptr, "user", 4) == 0, "role mismatch");
    assert_true(content.len == 8 && memcmp(content.ptr, "run tool", 8) == 0, "content mismatch");

    span_t id = {0};
    span_t name = {0};
    span_t args = {0};
    assert_true(extract_assistant_tool_call_fields(g_fake.request_bodies[1], g_fake.request_lens[1], &id, &name, &args),
                "assistant tool_calls missing from follow-up request");
    assert_true(id.len == 6 && memcmp(id.ptr, "call_1", 6) == 0, "tool id mismatch");
    assert_true(name.len == 3 && memcmp(name.ptr, "add", 3) == 0, "tool name mismatch");
    assert_true(args.len == 1 && memcmp(args.ptr, "1", 1) == 0, "tool args mismatch");

    llm_client_destroy(client);
    return true;
}

int main(void) {
    assert_true(test_tool_loop_includes_tool_calls(), "tool loop tool_calls request test failed");
    assert_true(test_tool_loop_params_passthrough(), "tool loop params passthrough test failed");
    assert_true(test_tool_loop_detects_repeat(), "tool loop repeat detection test failed");
    assert_true(test_tool_loop_max_turns(), "tool loop max turns test failed");
    assert_true(test_tool_loop_args_limit(), "tool loop args limit test failed");
    assert_true(test_tool_loop_output_limit(), "tool loop output limit test failed");
    printf("Tool loop request tests passed.\n");
    return 0;
}
