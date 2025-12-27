#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jstok.h"
#include "llm/llm.h"

static bool env_truthy(const char* name) {
    const char* value = getenv(name);
    if (!value || value[0] == '\0') return false;
    return !(value[0] == '0' && value[1] == '\0');
}

static void print_section(const char* title) { printf("\n== %s ==\n", title); }

static const char* or_dash(const char* value) {
    if (!value || value[0] == '\0') return "-";
    return value;
}

static void print_usage(const char* argv0) {
    printf("Usage: %s [--help]\n", argv0 ? argv0 : "chat_advanced");
    printf("Environment:\n");
    printf("  LLM_BASE_URL        base URL (default http://127.0.0.1:8080)\n");
    printf("  LLM_MODEL           model name (default gpt-4o)\n");
    printf("  LLM_API_KEY         API key (optional)\n");
    printf("  LLM_CA_BUNDLE       custom CA bundle path (optional)\n");
    printf("  LLM_CA_DIR          custom CA directory (optional)\n");
    printf("  LLM_CLIENT_CERT     client cert PEM (optional)\n");
    printf("  LLM_CLIENT_KEY      client key PEM (optional)\n");
    printf("  LLM_INSECURE        disable TLS verification (optional)\n");
    printf("  LLM_PROXY           proxy URL (optional)\n");
    printf("  LLM_NO_PROXY        no-proxy list (optional)\n");
    printf("  LLM_SHOW_REASONING  include reasoning deltas in stream output\n");
}

static const char* error_stage_str(llm_error_stage_t stage) {
    switch (stage) {
        case LLM_ERROR_STAGE_TRANSPORT:
            return "transport";
        case LLM_ERROR_STAGE_TLS:
            return "tls";
        case LLM_ERROR_STAGE_SSE:
            return "sse";
        case LLM_ERROR_STAGE_JSON:
            return "json";
        case LLM_ERROR_STAGE_PROTOCOL:
            return "protocol";
        case LLM_ERROR_STAGE_NONE:
        default:
            return "none";
    }
}

struct stream_state {
    size_t content_bytes;
    size_t reasoning_bytes;
};

static void print_error_detail(const char* label, llm_error_t err, const llm_error_detail_t* detail) {
    fprintf(stderr, "%s failed: %s\n", label, llm_errstr(err));
    if (!detail) return;
    fprintf(stderr, "stage=%s", error_stage_str(detail->stage));
    if (detail->has_http_status) {
        fprintf(stderr, " http=%ld", detail->http_status);
    }
    fprintf(stderr, "\n");
    if (detail->message && detail->message_len) {
        fprintf(stderr, "message: %.*s\n", (int)detail->message_len, detail->message);
    }
    if (detail->type && detail->type_len) {
        fprintf(stderr, "type: %.*s\n", (int)detail->type_len, detail->type);
    }
    if (detail->error_code && detail->error_code_len) {
        fprintf(stderr, "code: %.*s\n", (int)detail->error_code_len, detail->error_code);
    }
    if (detail->raw_body && detail->raw_body_len) {
        size_t cap = detail->raw_body_len > 256 ? 256 : detail->raw_body_len;
        fprintf(stderr, "raw_body: %.*s%s\n", (int)cap, detail->raw_body, cap < detail->raw_body_len ? "..." : "");
    }
}

static bool unescape_tool_args(const char* input, size_t input_len, char** out, size_t* out_len) {
    if (!input || !out || !out_len) return false;
    if (input_len > (size_t)INT_MAX) return false;
    if (input_len >= SIZE_MAX - 1) return false;

    char* buf = malloc(input_len + 1);
    if (!buf) return false;

    jstoktok_t tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = JSTOK_STRING;
    tok.start = 0;
    tok.end = (int)input_len;

    size_t unescaped_len = 0;
    if (jstok_unescape(input, &tok, buf, input_len + 1, &unescaped_len) != 0) {
        free(buf);
        return false;
    }
    buf[unescaped_len] = '\0';
    *out = buf;
    *out_len = unescaped_len;
    return true;
}

static char* dup_string(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len >= SIZE_MAX - 1) return NULL;
    char* out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static bool json_extract_string(const char* json, size_t len, const char* key, const char** out, size_t* out_len) {
    if (!json || !key || !out || !out_len) return false;
    if (len > (size_t)INT_MAX) return false;

    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return false;

    jstoktok_t* tokens = malloc((size_t)needed * sizeof(*tokens));
    if (!tokens) return false;

    jstok_init(&parser);
    int count = jstok_parse(&parser, json, (int)len, tokens, needed);
    bool ok = false;
    if (count > 0 && tokens[0].type == JSTOK_OBJECT) {
        int idx = jstok_object_get(json, tokens, count, 0, key);
        if (idx >= 0 && tokens[idx].type == JSTOK_STRING) {
            jstok_span_t sp = jstok_span(json, &tokens[idx]);
            *out = sp.p;
            *out_len = sp.n;
            ok = true;
        }
    }

    free(tokens);
    return ok;
}

static bool json_extract_int(const char* json, size_t len, const char* key, long long* out) {
    if (!json || !key || !out) return false;
    if (len > (size_t)INT_MAX) return false;

    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return false;

    jstoktok_t* tokens = malloc((size_t)needed * sizeof(*tokens));
    if (!tokens) return false;

    jstok_init(&parser);
    int count = jstok_parse(&parser, json, (int)len, tokens, needed);
    bool ok = false;
    if (count > 0 && tokens[0].type == JSTOK_OBJECT) {
        int idx = jstok_object_get(json, tokens, count, 0, key);
        if (idx >= 0 && tokens[idx].type == JSTOK_PRIMITIVE) {
            long long val = 0;
            if (jstok_atoi64(json, &tokens[idx], &val) == 0) {
                *out = val;
                ok = true;
            }
        }
    }

    free(tokens);
    return ok;
}

static bool build_tool_result_from_args(const llm_tool_call_t* call, const char* args, size_t args_len, char** out_json,
                                        size_t* out_len) {
    if (!call || !out_json || !out_len) return false;
    if (!call->name || call->name_len == 0) return false;
    if (args_len > 0 && !args) return false;

    bool ok = false;
    if (call->name_len == 11 && memcmp(call->name, "get_weather", 11) == 0) {
        const char* location = NULL;
        size_t location_len = 0;
        const char* reply = "{\"location\":\"unknown\",\"temperature_c\":0,\"condition\":\"unknown\"}";
        if (args && args_len > 0 && json_extract_string(args, args_len, "location", &location, &location_len)) {
            if (location_len == 5 && memcmp(location, "Tokyo", 5) == 0) {
                reply = "{\"location\":\"Tokyo\",\"temperature_c\":26,\"condition\":\"clear\"}";
            } else if (location_len == 6 && memcmp(location, "Berlin", 6) == 0) {
                reply = "{\"location\":\"Berlin\",\"temperature_c\":18,\"condition\":\"overcast\"}";
            }
        }
        char* out = dup_string(reply);
        if (out) {
            *out_json = out;
            *out_len = strlen(out);
            ok = true;
        }
    } else if (call->name_len == 9 && memcmp(call->name, "roll_dice", 9) == 0) {
        long long sides = 6;
        if (args && args_len > 0) {
            (void)json_extract_int(args, args_len, "sides", &sides);
        }
        if (sides < 2 || sides > 1000) sides = 6;
        long long result = (sides + 1) / 2;
        char* out = malloc(64);
        if (out) {
            int written = snprintf(out, 64, "{\"sides\":%lld,\"result\":%lld}", sides, result);
            if (written > 0 && (size_t)written < 64) {
                *out_json = out;
                *out_len = (size_t)written;
                ok = true;
            } else {
                free(out);
            }
        }
    } else {
        const char* reply = "{\"error\":\"unknown_tool\"}";
        char* out = dup_string(reply);
        if (out) {
            *out_json = out;
            *out_len = strlen(out);
            ok = true;
        }
    }

    return ok;
}

static bool build_assistant_content(const llm_chat_result_t* result, char** out, size_t* out_len) {
    if (!out || !out_len) return false;
    *out = NULL;
    *out_len = 0;
    if (!result) return false;

    size_t total = 0;
    if (result->content && result->content_len) {
        total += result->content_len;
    }
    if (result->reasoning_content && result->reasoning_content_len) {
        if (total > SIZE_MAX - result->reasoning_content_len) return false;
        total += result->reasoning_content_len;
    }
    if (total == 0) return true;
    if (total >= SIZE_MAX - 1) return false;

    char* buf = malloc(total + 1);
    if (!buf) return false;

    size_t offset = 0;
    if (result->content && result->content_len) {
        memcpy(buf + offset, result->content, result->content_len);
        offset += result->content_len;
    }
    if (result->reasoning_content && result->reasoning_content_len) {
        memcpy(buf + offset, result->reasoning_content, result->reasoning_content_len);
        offset += result->reasoning_content_len;
    }
    buf[total] = '\0';
    *out = buf;
    *out_len = total;
    return true;
}

static void on_content_delta(void* user_data, const char* delta, size_t len) {
    struct stream_state* state = user_data;
    if (state) state->content_bytes += len;
    fwrite(delta, 1, len, stdout);
    fflush(stdout);
}

static void on_reasoning_delta(void* user_data, const char* delta, size_t len) {
    struct stream_state* state = user_data;
    if (state) state->reasoning_bytes += len;
    fprintf(stderr, "[reasoning] %.*s", (int)len, delta);
    fflush(stderr);
}

static void on_usage(void* user_data, const llm_usage_t* usage) {
    (void)user_data;
    if (!usage->has_prompt_tokens && !usage->has_completion_tokens && !usage->has_total_tokens) return;
    fprintf(stderr, "\n[usage]");
    if (usage->has_prompt_tokens) {
        fprintf(stderr, " prompt=%zu", usage->prompt_tokens);
    }
    if (usage->has_completion_tokens) {
        fprintf(stderr, " completion=%zu", usage->completion_tokens);
    }
    if (usage->has_total_tokens) {
        fprintf(stderr, " total=%zu", usage->total_tokens);
    }
    fprintf(stderr, "\n");
}

static void on_finish_reason(void* user_data, llm_finish_reason_t reason) {
    (void)user_data;
    fprintf(stderr, "\n[finish] %s\n", llm_finish_reason_to_string(reason));
}

int main(int argc, char** argv) {
    if (argc > 1) {
        if (argc == 2 && strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    const char* base_url = getenv("LLM_BASE_URL");
    if (!base_url || base_url[0] == '\0') base_url = "http://127.0.0.1:8080";

    const char* model_name = getenv("LLM_MODEL");
    if (!model_name || model_name[0] == '\0') model_name = "gpt-4o";
    bool show_reasoning = env_truthy("LLM_SHOW_REASONING");
    bool ok = true;

    llm_timeout_t timeout = {
        .connect_timeout_ms = 5000,
        .overall_timeout_ms = 30000,
        .read_idle_timeout_ms = 10000,
    };
    llm_limits_t limits = {
        .max_response_bytes = 2 * 1024 * 1024,
        .max_line_bytes = 256 * 1024,
        .max_frame_bytes = 256 * 1024,
        .max_sse_buffer_bytes = 2 * 1024 * 1024,
        .max_tool_args_bytes_per_call = 16 * 1024,
        .max_tool_args_bytes_per_turn = 64 * 1024,
        .max_tool_output_bytes_total = 64 * 1024,
        .max_embedding_input_bytes = 256 * 1024,
        .max_embedding_inputs = 128,
        .max_content_parts = 32,
        .max_content_bytes = 64 * 1024,
    };
    llm_client_init_opts_t init_opts = {.enable_last_error = true};
    llm_model_t model = {.name = model_name};
    const char* default_headers[] = {"User-Agent: desi-advanced-example/1.0", "X-Client-Mode: advanced"};

    llm_client_t* client =
        llm_client_create_with_headers_opts(base_url, &model, &timeout, &limits, default_headers, 2, &init_opts);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }

    const char* api_key = getenv("LLM_API_KEY");
    if (api_key && api_key[0] != '\0') {
        if (!llm_client_set_api_key(client, api_key)) {
            fprintf(stderr, "Failed to set API key\n");
            llm_client_destroy(client);
            return 1;
        }
    }

    llm_tls_config_t tls = {0};
    bool have_tls = false;
    const char* ca_bundle = getenv("LLM_CA_BUNDLE");
    const char* ca_dir = getenv("LLM_CA_DIR");
    const char* client_cert = getenv("LLM_CLIENT_CERT");
    const char* client_key = getenv("LLM_CLIENT_KEY");
    if (ca_bundle && ca_bundle[0] != '\0') {
        tls.ca_bundle_path = ca_bundle;
        have_tls = true;
    }
    if (ca_dir && ca_dir[0] != '\0') {
        tls.ca_dir_path = ca_dir;
        have_tls = true;
    }
    if (client_cert && client_cert[0] != '\0') {
        tls.client_cert_path = client_cert;
        have_tls = true;
    }
    if (client_key && client_key[0] != '\0') {
        tls.client_key_path = client_key;
        have_tls = true;
    }
    if (env_truthy("LLM_INSECURE")) {
        tls.insecure = true;
        have_tls = true;
    }
    if (have_tls && !llm_client_set_tls_config(client, &tls)) {
        fprintf(stderr, "Failed to apply TLS config\n");
        llm_client_destroy(client);
        return 1;
    }

    const char* proxy = getenv("LLM_PROXY");
    if (proxy && proxy[0] != '\0') {
        if (!llm_client_set_proxy(client, proxy)) {
            fprintf(stderr, "Failed to set proxy\n");
            llm_client_destroy(client);
            return 1;
        }
    }
    const char* no_proxy = getenv("LLM_NO_PROXY");
    if (no_proxy && no_proxy[0] != '\0') {
        if (!llm_client_set_no_proxy(client, no_proxy)) {
            fprintf(stderr, "Failed to set no_proxy\n");
            llm_client_destroy(client);
            return 1;
        }
    }

    printf("desi advanced showcase\n");
    print_section("Configuration");
    printf("Base URL: %s\n", base_url);
    printf("Model: %s\n", model_name);
    printf("API key: %s\n", (api_key && api_key[0] != '\0') ? "set" : "not set");
    printf("Timeouts: connect=%ldms overall=%ldms idle=%ldms\n", timeout.connect_timeout_ms, timeout.overall_timeout_ms,
           timeout.read_idle_timeout_ms);
    printf("Limits: response=%zu line=%zu frame=%zu tool_args/call=%zu\n", limits.max_response_bytes,
           limits.max_line_bytes, limits.max_frame_bytes, limits.max_tool_args_bytes_per_call);
    if (have_tls) {
        printf("TLS: ca_bundle=%s ca_dir=%s client_cert=%s client_key=%s insecure=%s\n", or_dash(ca_bundle),
               or_dash(ca_dir), or_dash(client_cert), or_dash(client_key), tls.insecure ? "on" : "off");
    } else {
        printf("TLS: default trust store\n");
    }
    printf("Proxy: %s\n", or_dash(proxy));
    printf("No proxy: %s\n", or_dash(no_proxy));
    printf("Reasoning deltas: %s\n", show_reasoning ? "on" : "off");

    print_section("Health Check");
    llm_error_detail_t detail = {0};
    llm_error_t err = llm_health_ex(client, &detail);
    if (err != LLM_ERR_NONE) {
        print_error_detail("Health check", err, &detail);
        llm_error_detail_free(&detail);
        llm_client_destroy(client);
        return 1;
    }
    llm_error_detail_free(&detail);

    llm_request_opts_t req = {0};
    req.has_temperature = true;
    req.temperature = 0.25;
    req.has_top_p = true;
    req.top_p = 0.75;
    req.has_max_tokens = true;
    req.max_tokens = 256;
    req.has_seed = true;
    req.seed = 4242;
    const char* stops[] = {"<<END>>"};
    const size_t stop_lens[] = {7};
    req.stop_list = stops;
    req.stop_lens = stop_lens;
    req.stop_count = 1;

    char params_json[512];
    size_t params_len = 0;
    if (!llm_request_opts_json_write(&req, params_json, sizeof(params_json), 4, 32, &params_len)) {
        fprintf(stderr, "Failed to build request options JSON\n");
        llm_client_destroy(client);
        return 1;
    }
    params_json[params_len] = '\0';

    const char* tooling_json =
        "{"
        "\"tools\":["
        "{"
        "\"type\":\"function\","
        "\"function\":{"
        "\"name\":\"get_weather\","
        "\"description\":\"Get current weather for a city\","
        "\"parameters\":{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"location\":{\"type\":\"string\",\"description\":\"City name\"}"
        "},"
        "\"required\":[\"location\"]"
        "}"
        "}"
        "},"
        "{"
        "\"type\":\"function\","
        "\"function\":{"
        "\"name\":\"roll_dice\","
        "\"description\":\"Return a deterministic dice roll\","
        "\"parameters\":{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"sides\":{\"type\":\"integer\",\"minimum\":2,\"maximum\":1000}"
        "},"
        "\"required\":[\"sides\"]"
        "}"
        "}"
        "}"
        "]"
        "}";

    const char* sys = "You are a helpful demo assistant. Use tools when you need concrete data.";
    const char* usr = "Use get_weather for Tokyo and roll_dice with sides=12, then write a short, friendly update.";
    llm_message_t initial_msgs[] = {
        {.role = LLM_ROLE_SYSTEM, .content = sys, .content_len = strlen(sys)},
        {.role = LLM_ROLE_USER, .content = usr, .content_len = strlen(usr)},
    };

    llm_stream_callbacks_t cbs = {0};
    struct stream_state stream_state = {0};
    cbs.user_data = &stream_state;
    cbs.on_content_delta = on_content_delta;
    if (show_reasoning) {
        cbs.on_reasoning_delta = on_reasoning_delta;
    }
    cbs.on_usage = on_usage;
    cbs.on_finish_reason = on_finish_reason;
    cbs.include_usage = true;

    print_section("Tool Call Request");
    printf("Tools: get_weather(location), roll_dice(sides)\n");
    printf("Request options: %s\n", params_json);
    printf("System: %s\n", sys);
    printf("User: %s\n", usr);

    llm_chat_result_t result = {0};
    const char* request_headers[] = {"X-Request-Id: tool-call-1"};
    err = llm_chat_with_headers_ex(client, initial_msgs, 2, params_json, tooling_json, NULL, &result, request_headers,
                                   1, &detail);
    if (err != LLM_ERR_NONE) {
        print_error_detail("Initial tool call", err, &detail);
        llm_error_detail_free(&detail);
        llm_client_destroy(client);
        return 1;
    }
    llm_error_detail_free(&detail);

    bool have_tool_calls = (result.finish_reason == LLM_FINISH_REASON_TOOL_CALLS && result.tool_calls_count > 0 &&
                            result.tool_calls_json && result.tool_calls_json_len > 0);
    if (!have_tool_calls) {
        printf("No tool calls returned; showing the assistant reply from the initial call.\n");
        if (result.content && result.content_len > 0) {
            printf("Assistant: %.*s\n", (int)result.content_len, result.content);
        } else {
            printf("Assistant: (no content)\n");
        }
        llm_chat_result_free(&result);
    } else {
        print_section("Tool Calls");
        printf("Tool calls returned: %zu\n", result.tool_calls_count);

        char* assistant_content = NULL;
        size_t assistant_content_len = 0;
        if (!build_assistant_content(&result, &assistant_content, &assistant_content_len)) {
            fprintf(stderr, "Failed to build assistant content\n");
            llm_chat_result_free(&result);
            llm_client_destroy(client);
            return 1;
        }

        size_t followup_count = 2 + 1 + result.tool_calls_count;
        llm_message_t* followup_msgs = calloc(followup_count, sizeof(*followup_msgs));
        if (!followup_msgs) {
            fprintf(stderr, "Failed to allocate follow-up messages\n");
            free(assistant_content);
            llm_chat_result_free(&result);
            llm_client_destroy(client);
            return 1;
        }

        followup_msgs[0] = initial_msgs[0];
        followup_msgs[1] = initial_msgs[1];
        followup_msgs[2].role = LLM_ROLE_ASSISTANT;
        followup_msgs[2].content = assistant_content;
        followup_msgs[2].content_len = assistant_content_len;
        followup_msgs[2].tool_calls_json = result.tool_calls_json;
        followup_msgs[2].tool_calls_json_len = result.tool_calls_json_len;

        char** tool_outputs = calloc(result.tool_calls_count, sizeof(*tool_outputs));
        if (!tool_outputs) {
            fprintf(stderr, "Failed to allocate tool outputs\n");
            free(assistant_content);
            free(followup_msgs);
            llm_chat_result_free(&result);
            llm_client_destroy(client);
            return 1;
        }

        bool tool_ok = true;
        for (size_t i = 0; i < result.tool_calls_count; i++) {
            const llm_tool_call_t* call = &result.tool_calls[i];
            if (!call->id || call->id_len == 0 || !call->name || call->name_len == 0) {
                fprintf(stderr, "Tool call %zu missing id or name\n", i + 1);
                tool_ok = false;
                break;
            }
            if (!call->arguments) {
                fprintf(stderr, "Tool call %zu missing arguments\n", i + 1);
                tool_ok = false;
                break;
            }
            char* args = NULL;
            size_t args_len = 0;
            if (!unescape_tool_args(call->arguments, call->arguments_len, &args, &args_len)) {
                fprintf(stderr, "Tool call %zu arguments are invalid JSON\n", i + 1);
                tool_ok = false;
                break;
            }
            printf("Tool call %zu: %.*s args=%.*s\n", i + 1, (int)call->name_len, call->name, (int)args_len, args);

            char* tool_json = NULL;
            size_t tool_len = 0;
            if (!build_tool_result_from_args(call, args, args_len, &tool_json, &tool_len)) {
                free(args);
                tool_ok = false;
                break;
            }
            free(args);
            printf("Tool result %zu: %.*s\n", i + 1, (int)tool_len, tool_json);
            tool_outputs[i] = tool_json;
            size_t msg_idx = 3 + i;
            if (!llm_tool_message_init(&followup_msgs[msg_idx], tool_json, tool_len, call->id, call->id_len, call->name,
                                       call->name_len)) {
                tool_ok = false;
                break;
            }
        }

        if (!tool_ok) {
            fprintf(stderr, "Tool dispatch failed\n");
            for (size_t i = 0; i < result.tool_calls_count; i++) {
                free(tool_outputs[i]);
            }
            free(tool_outputs);
            free(assistant_content);
            free(followup_msgs);
            llm_chat_result_free(&result);
            llm_client_destroy(client);
            return 1;
        }

        print_section("Final Answer");
        llm_chat_result_t final_result = {0};
        const char* final_headers[] = {"X-Request-Id: tool-call-final"};
        err = llm_chat_with_headers_ex(client, followup_msgs, followup_count, params_json, NULL, NULL, &final_result,
                                       final_headers, 1, &detail);
        if (err != LLM_ERR_NONE) {
            print_error_detail("Final answer", err, &detail);
            llm_error_detail_free(&detail);
            ok = false;
        } else {
            printf("Finish: %s\n", llm_finish_reason_to_string(final_result.finish_reason));
            if (final_result.content && final_result.content_len > 0) {
                printf("Assistant: %.*s\n", (int)final_result.content_len, final_result.content);
            } else {
                printf("Assistant: (no content)\n");
            }
            llm_error_detail_free(&detail);
        }
        llm_chat_result_free(&final_result);

        for (size_t i = 0; i < result.tool_calls_count; i++) {
            free(tool_outputs[i]);
        }
        free(tool_outputs);
        free(assistant_content);
        free(followup_msgs);
        llm_chat_result_free(&result);
    }

    print_section("Streaming Demo");
    const char* stream_prompt = "In one sentence, describe desi's core design philosophy.";
    llm_message_t stream_msgs[] = {
        {.role = LLM_ROLE_USER, .content = stream_prompt, .content_len = strlen(stream_prompt)},
    };
    const char* stream_headers[] = {"X-Request-Id: stream-demo"};
    stream_state.content_bytes = 0;
    stream_state.reasoning_bytes = 0;
    printf("Assistant: ");
    fflush(stdout);
    err = llm_chat_stream_with_headers_detail_ex(client, stream_msgs, 1, params_json, NULL, NULL, &cbs, NULL, NULL,
                                                 stream_headers, 1, &detail);
    printf("\n");
    if (err != LLM_ERR_NONE) {
        print_error_detail("Streaming demo", err, &detail);
        llm_error_detail_free(&detail);
        ok = false;
    } else {
        llm_error_detail_free(&detail);
        if (stream_state.content_bytes == 0) {
            printf("(no streamed content received)\n");
        }
    }

    llm_client_destroy(client);
    return ok ? 0 : 1;
}
