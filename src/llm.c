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
};

llm_client_t* llm_client_create(const char* base_url, const llm_model_t* model, const llm_timeout_t* timeout,
                                const llm_limits_t* limits) {
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
        client->limits.max_tool_args_bytes_per_call = 1024 * 1024;
    }

    return client;
}

void llm_client_destroy(llm_client_t* client) {
    if (client) {
        free(client->base_url);
        free((char*)client->model.name);
        free(client);
    }
}

bool llm_health(llm_client_t* client) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/health", client->base_url);
    char* body = NULL;
    size_t len = 0;
    bool ok = http_get(url, client->timeout.connect_timeout_ms, 1024, &body, &len);
    free(body);
    return ok;
}

char** llm_models_list(llm_client_t* client, size_t* count) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/models", client->base_url);
    char* body = NULL;
    size_t len = 0;
    if (!http_get(url, client->timeout.connect_timeout_ms, client->limits.max_response_bytes, &body, &len)) {
        return NULL;
    }

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

void llm_models_list_free(char** models, size_t count) {
    if (models) {
        for (size_t i = 0; i < count; i++) {
            free(models[i]);
        }
        free(models);
    }
}

bool llm_props_get(llm_client_t* client, const char** json, size_t* len) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/health", client->base_url);
    return http_get(url, client->timeout.connect_timeout_ms, client->limits.max_response_bytes, (char**)json, len);
}

// Forward declarations from other modules
int parse_chat_response(const char* json, size_t len, llm_chat_result_t* result);
int parse_chat_chunk(const char* json, size_t len, llm_chat_chunk_delta_t* delta);
int parse_completions_response(const char* json, size_t len, const char*** texts, size_t* count);

bool llm_completions(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                     const char*** texts, size_t* count) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/completions", client->base_url);

    char* request_json = build_completions_request(client->model.name, prompt, prompt_len, params_json);
    if (!request_json) return false;

    char* response_body = NULL;
    size_t response_len = 0;
    bool ok = http_post(url, request_json, client->timeout.overall_timeout_ms, client->limits.max_response_bytes,
                        &response_body, &response_len);
    free(request_json);

    if (ok) {
        int res = parse_completions_response(response_body, response_len, texts, count);
        if (res < 0) {
            free(response_body);
            ok = false;
        } else {
            free(response_body);  // parse_completions_response now makes copies
        }
    }
    return ok;
}

void llm_completions_free(const char** texts, size_t count) {
    if (texts) {
        for (size_t i = 0; i < count; i++) {
            free((char*)texts[i]);
        }
        free(texts);
    }
}

bool llm_chat(llm_client_t* client, const llm_message_t* messages, size_t messages_count, const char* params_json,
              const char* tooling_json, const char* response_format_json, llm_chat_result_t* result) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", client->base_url);

    char* request_json = build_chat_request(client->model.name, messages, messages_count, false, params_json,
                                            tooling_json, response_format_json);
    if (!request_json) return false;

    char* response_body = NULL;
    size_t response_len = 0;
    bool ok = http_post(url, request_json, client->timeout.overall_timeout_ms, client->limits.max_response_bytes,
                        &response_body, &response_len);
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

void llm_chat_result_free(llm_chat_result_t* result) {
    if (result) {
        free(result->tool_calls);
        free(result->_internal);
        memset(result, 0, sizeof(*result));
    }
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
    struct tool_call_accumulator* accums;
    size_t accums_count;
    size_t max_tool_args;
};

static void on_sse_data(void* user_data, span_t line) {
    struct stream_ctx* ctx = user_data;
    llm_chat_chunk_delta_t delta;
    if (parse_chat_chunk(line.ptr, line.len, &delta) == 0) {
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
} curl_stream_ctx;

static void curl_stream_cb(const char* chunk, size_t len, void* user_data) {
    curl_stream_ctx* cs = user_data;
    sse_feed(cs->sse, chunk, len);
}

bool llm_chat_stream(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                     const char* params_json, const char* tooling_json, const char* response_format_json,
                     const llm_stream_callbacks_t* callbacks) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", client->base_url);

    char* request_json = build_chat_request(client->model.name, messages, messages_count, true, params_json,
                                            tooling_json, response_format_json);
    if (!request_json) return false;

    struct stream_ctx ctx = {callbacks, NULL, 0, client->limits.max_tool_args_bytes_per_call};
    sse_parser_t* sse = sse_create(client->limits.max_line_bytes, client->limits.max_response_bytes);
    sse_set_callback(sse, on_sse_data, &ctx);

    curl_stream_ctx cs = {sse, &ctx};
    bool ok = http_post_stream(url, request_json, client->timeout.overall_timeout_ms,
                               client->timeout.read_idle_timeout_ms, curl_stream_cb, &cs);

    for (size_t i = 0; i < ctx.accums_count; i++) {
        accum_free(&ctx.accums[i]);
    }
    free(ctx.accums);
    sse_destroy(sse);
    free(request_json);

    return ok;
}

// Tool loop implementation is usually complex, let's put a simplified version here
bool llm_tool_loop_run(llm_client_t* client, const llm_message_t* initial_messages, size_t initial_count,
                       const char* tooling_json, llm_tool_dispatch_cb dispatch, void* dispatch_user_data,
                       size_t max_turns) {
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
        if (!llm_chat(client, history, history_count, NULL, tooling_json, NULL, &result)) {
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
