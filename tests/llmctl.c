#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm.h"

static void log_msg(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("[TEST] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

static void assert_msg(bool cond, const char* fmt, ...) {
    if (!cond) {
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "FAIL: ");
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
        exit(1);
    }
}

#define LOG log_msg
#define ASSERT assert_msg

#define TEST_SKIP 77

static void print_sep(const char* title) { printf("\n===== %s =====\n", title); }

// Global config
static const char* base_url = "http://10.0.5.69:8080";
static const char* model_name = NULL;

static bool live_tests_enabled(void) {
    const char* env = getenv("LLM_LIVE_TESTS");
    if (!env || env[0] == '\0') return false;
    return !(env[0] == '0' && env[1] == '\0');
}

static bool live_should_skip(const llm_error_detail_t* detail) {
    if (!detail) return true;
    if (detail->stage == LLM_ERROR_STAGE_TRANSPORT || detail->stage == LLM_ERROR_STAGE_TLS) return true;
    if (!detail->has_http_status || detail->http_status == 0) return true;
    return false;
}

static int test_health(llm_client_t* client) {
    print_sep("1) GET /health");
    llm_error_detail_t detail = {0};
    llm_error_t err = llm_health_ex(client, &detail);
    if (err == LLM_ERR_NONE) {
        LOG("Health check passed");
        llm_error_detail_free(&detail);
        return 0;
    }
    bool skip = live_should_skip(&detail);
    llm_error_detail_free(&detail);
    if (skip) {
        LOG("Skipping live tests (server unreachable)");
        return TEST_SKIP;
    }
    return 1;
}

static void test_models(llm_client_t* client) {
    print_sep("2) GET /v1/models");
    size_t count = 0;
    char** models = llm_models_list(client, &count);
    ASSERT(models != NULL, "Failed to list models");
    ASSERT(count > 0, "No models found");

    LOG("Found %zu models:", count);
    for (size_t i = 0; i < count; i++) {
        LOG(" - %s", models[i]);
    }

    // Auto-select first model if not set
    if (!model_name) {
        model_name = strdup(models[0]);
        LOG("Auto-selected model: %s", model_name);
    }

    llm_models_list_free(models, count);
}

static void test_props(llm_client_t* client) {
    print_sep("4) GET /props");
    const char* json = NULL;
    size_t len = 0;
    if (llm_props_get(client, &json, &len)) {
        LOG("Props received (%zu bytes)", len);
        ASSERT(len > 0 && json[0] == '{', "Invalid props response");
        free((char*)json);
    } else {
        LOG("Skipping /props (failed or not supported)");
    }
}

static void test_completions_basic(llm_client_t* client) {
    print_sep("6) POST /v1/completions basic");
    const char* prompt = "one\ntwo\n";
    const char* params = "{\"max_tokens\": 32, \"temperature\": 0, \"stop\": [\"\\n\"]}";

    llm_completions_result_t res = {0};

    if (llm_completions(client, prompt, strlen(prompt), params, &res)) {
        LOG("Completions received: %zu", res.choices_count);
        for (size_t i = 0; i < res.choices_count; i++) {
            LOG("Choice %zu: '%.*s'", i, (int)res.choices[i].text_len, res.choices[i].text);
        }
        llm_completions_free(&res);
    } else {
        ASSERT(false, "Completions failed");
    }
}

static void test_chat_basic(llm_client_t* client) {
    print_sep("9) /v1/chat/completions basic");
    const char* sys = "Return exactly: OK";
    const char* usr = "Do it";
    llm_message_t msgs[] = {{LLM_ROLE_SYSTEM, sys, strlen(sys), NULL, 0, NULL, 0, NULL, 0, NULL, 0},
                            {LLM_ROLE_USER, usr, strlen(usr), NULL, 0, NULL, 0, NULL, 0, NULL, 0}};

    llm_chat_result_t res;
    if (llm_chat(client, msgs, 2, "{\"temperature\": 0}", NULL, NULL, &res)) {
        LOG("Finish: %s", llm_finish_reason_to_string(res.finish_reason));
        if (res.content) {
            LOG("Content: %.*s", (int)res.content_len, res.content);
            ASSERT(res.content_len >= 2 && memcmp(res.content, "OK", 2) == 0, "Expected OK in content");
        }
        llm_chat_result_free(&res);
    } else {
        ASSERT(false, "Chat failed");
    }
}

// Streaming callback
static void stream_content_cb(void* user_data, const char* delta, size_t len) {
    (void)user_data;
    printf("%.*s", (int)len, delta);
    fflush(stdout);
}

static void test_chat_streaming(llm_client_t* client) {
    print_sep("10) /v1/chat streaming");
    const char* content = "Output exactly: OK";
    llm_message_t msgs[] = {{LLM_ROLE_USER, content, strlen(content), NULL, 0, NULL, 0, NULL, 0, NULL, 0}};

    llm_stream_callbacks_t cbs = {0};
    cbs.on_content_delta = stream_content_cb;

    printf("Stream output: ");
    if (!llm_chat_stream(client, msgs, 1, "{\"temperature\": 0}", NULL, NULL, &cbs)) {
        ASSERT(false, "Streaming failed");
    }
    printf("\n");
}

static void test_tools_call(llm_client_t* client) {
    print_sep("12) Tools: single tool_call");
    const char* tooling =
        "{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"add\",\"description\":\"Add two "
        "integers\",\"parameters\":{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},\"b\":{\"type\":"
        "\"integer\"}},\"required\":[\"a\",\"b\"]}}}]}";

    const char* sys_content = "Call tool add with a=12 and b=30.";
    const char* usr_content = "Go";

    llm_message_t msgs[] = {
        {LLM_ROLE_SYSTEM, strdup(sys_content), strlen(sys_content), NULL, 0, NULL, 0, NULL, 0, NULL, 0},
        {LLM_ROLE_USER, strdup(usr_content), strlen(usr_content), NULL, 0, NULL, 0, NULL, 0, NULL, 0}};

    // Check for strdup failures
    if (!msgs[0].content || !msgs[1].content) {
        if (msgs[0].content) free((char*)msgs[0].content);
        if (msgs[1].content) free((char*)msgs[1].content);
        ASSERT(false, "Failed to strdup message content for test_tools_call");
        return;
    }

    llm_chat_result_t res;
    if (llm_chat(client, msgs, 2, "{\"temperature\": 0}", tooling, NULL, &res)) {
        ASSERT(res.finish_reason == LLM_FINISH_REASON_TOOL_CALLS, "Expected finish_reason=tool_calls");
        ASSERT(res.tool_calls_count == 1, "Expected 1 tool call");
        ASSERT(res.tool_calls[0].name_len == 3 && memcmp(res.tool_calls[0].name, "add", 3) == 0,
               "Expected tool name 'add'");
        LOG("Tool called: %.*s(%.*s)", (int)res.tool_calls[0].name_len, res.tool_calls[0].name,
            (int)res.tool_calls[0].arguments_len, res.tool_calls[0].arguments);
        llm_chat_result_free(&res);
    } else {
        ASSERT(false, "Tool chat failed");
    }

    // Free duplicated content
    free((char*)msgs[0].content);
    free((char*)msgs[1].content);
}

// Tool loop dispatcher
static bool math_dispatch(void* user_data, const char* name, size_t name_len, const char* args, size_t args_len,
                          char** out_json, size_t* out_len) {
    (void)user_data;
    LOG("Dispatching tool: %.*s args: %.*s", (int)name_len, name, (int)args_len, args);

    if (name_len == 3 && memcmp(name, "add", 3) == 0) {
        // Mock result: 12+30=42
        if (memmem(args, args_len, "12", 2) && memmem(args, args_len, "30", 2)) {
            *out_json = strdup("42");
            *out_len = 2;
            return true;
        }
    }
    if (name_len == 3 && memcmp(name, "mul", 3) == 0) {
        // Mock result: 42*2=84
        if (memmem(args, args_len, "42", 2) && memmem(args, args_len, "2", 1)) {
            *out_json = strdup("84");
            *out_len = 2;
            return true;
        }
    }

    // Fallback
    *out_json = strdup("0");
    *out_len = 1;
    return true;
}

static void test_tool_loop(llm_client_t* client) {
    print_sep("13) Tool loop: add then mul");
    const char* tooling =
        "{\"tools\":["
        "{\"type\":\"function\",\"function\":{\"name\":\"add\",\"description\":\"Add two integers\",\"parameters\":"
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},\"b\":{\"type\":\"integer\"}},"
        "\"required\":[\"a\",\"b\"]}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"mul\",\"description\":\"Multiply two integers\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},\"b\":{\"type\":\"integer\"}}"
        ","
        "\"required\":[\"a\",\"b\"]}}}"
        "]}";

    const char* sys_content =
        "Use tools only. First call add with a=12 and b=30. Then call mul with a=<result> and b=2. "
        "Finally output RESULT=<val>.";
    const char* usr_content = "Go.";
    llm_message_t msgs[] = {
        {LLM_ROLE_SYSTEM, strdup(sys_content), strlen(sys_content), NULL, 0, NULL, 0, NULL, 0, NULL, 0},
        {LLM_ROLE_USER, strdup(usr_content), strlen(usr_content), NULL, 0, NULL, 0, NULL, 0, NULL, 0}};

    // Check for strdup failures
    if (!msgs[0].content || !msgs[1].content) {
        if (msgs[0].content) free((char*)msgs[0].content);
        if (msgs[1].content) free((char*)msgs[1].content);
        ASSERT(false, "Failed to strdup message content for test_tool_loop");
        return;
    }

    if (llm_tool_loop_run(client, msgs, 2, NULL, tooling, NULL, math_dispatch, NULL, 5)) {
        LOG("Tool loop completed successfully");
    } else {
        ASSERT(false, "Tool loop failed");
    }

    free((char*)msgs[0].content);
    free((char*)msgs[1].content);
}
static void test_json_mode(llm_client_t* client) {
    print_sep("18) response_format json_object");
    llm_message_t msgs[] = {
        {LLM_ROLE_USER, "Return JSON with key 'status'='ok'", 34, NULL, 0, NULL, 0, NULL, 0, NULL, 0}};

    llm_chat_result_t res;
    if (llm_chat(client, msgs, 1, "{\"temperature\": 0}", NULL, "{\"type\": \"json_object\"}", &res)) {
        LOG("Content: %.*s", (int)res.content_len, res.content);
        ASSERT(res.content_len > 0 && res.content[0] == '{', "Expected JSON output");
        llm_chat_result_free(&res);
    } else {
        ASSERT(false, "JSON mode failed");
    }
}

int main(void) {
    if (!live_tests_enabled()) {
        LOG("Skipping live tests (set LLM_LIVE_TESTS=1)");
        return TEST_SKIP;
    }

    const char* env_url = getenv("LLM_BASE_URL");
    if (env_url) base_url = env_url;

    const char* env_model = getenv("LLM_MODEL");
    if (env_model) model_name = strdup(env_model);

    LOG("Starting Smoke Tests against %s", base_url);

    llm_model_t dummy_model = {"dummy"};
    llm_client_t* client = llm_client_create(base_url, &dummy_model, NULL, NULL);
    ASSERT(client != NULL, "Client creation failed");

    int health = test_health(client);
    if (health == TEST_SKIP) {
        llm_client_destroy(client);
        return TEST_SKIP;
    }
    ASSERT(health == 0, "Health check failed");
    test_models(client);

    llm_client_destroy(client);
    llm_model_t real_model;
    real_model.name = model_name;
    client = llm_client_create(base_url, &real_model, NULL, NULL);
    ASSERT(client != NULL, "Client recreation failed");

    test_props(client);
    test_completions_basic(client);
    test_chat_basic(client);
    test_chat_streaming(client);
    test_tools_call(client);
    test_tool_loop(client);
    test_json_mode(client);

    llm_client_destroy(client);
    LOG("All Smoke Tests Passed!");
    return 0;
}
