#include <stdarg.h>
#include <stdio.h>

#include "llm/internal.h"
#include "llm/llm.h"

static void append_lit(struct growbuf* b, const char* lit) { growbuf_append(b, lit, strlen(lit), 0); }

static void append_char(struct growbuf* b, char c) { growbuf_append(b, &c, 1, 0); }

static void append_json_string(struct growbuf* b, const char* str, size_t len) {
    append_char(b, '"');
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '"':
                append_lit(b, "\\\"");
                break;
            case '\\':
                append_lit(b, "\\\\");
                break;
            case '\b':
                append_lit(b, "\\b");
                break;
            case '\f':
                append_lit(b, "\\f");
                break;
            case '\n':
                append_lit(b, "\\n");
                break;
            case '\r':
                append_lit(b, "\\r");
                break;
            case '\t':
                append_lit(b, "\\t");
                break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    append_lit(b, buf);
                } else {
                    append_char(b, c);
                }
                break;
        }
    }
    append_char(b, '"');
}

static void append_json_raw(struct growbuf* b, const char* json, size_t len) {
    if (json && len > 0) {
        growbuf_append(b, json, len, 0);
    } else {
        append_lit(b, "null");
    }
}

// Internal header for these functions?
// For now they are only used in this file or can be made available to others if needed.

char* build_chat_request(const char* model, const llm_message_t* messages, size_t messages_count, bool stream,
                         bool include_usage, const char* params_json, const char* tooling_json,
                         const char* response_format_json) {
    struct growbuf b;
    growbuf_init(&b, 4096);

    append_lit(&b, "{\"model\":");
    append_json_string(&b, model, strlen(model));

    append_lit(&b, ",\"messages\":[");
    for (size_t i = 0; i < messages_count; i++) {
        if (i > 0) append_char(&b, ',');
        append_lit(&b, "{\"role\":");
        const char* role_str = "user";
        switch (messages[i].role) {
            case LLM_ROLE_SYSTEM:
                role_str = "system";
                break;
            case LLM_ROLE_USER:
                role_str = "user";
                break;
            case LLM_ROLE_ASSISTANT:
                role_str = "assistant";
                break;
            case LLM_ROLE_TOOL:
                role_str = "tool";
                break;
        }
        append_json_string(&b, role_str, strlen(role_str));

        if (messages[i].content) {
            append_lit(&b, ",\"content\":");
            append_json_string(&b, messages[i].content, messages[i].content_len);
        } else {
            append_lit(&b, ",\"content\":null");
        }

        if (messages[i].role == LLM_ROLE_ASSISTANT && messages[i].tool_calls_json &&
            messages[i].tool_calls_json_len > 0) {
            append_lit(&b, ",\"tool_calls\":");
            growbuf_append(&b, messages[i].tool_calls_json, messages[i].tool_calls_json_len, 0);
        }

        if (messages[i].role == LLM_ROLE_TOOL && messages[i].tool_call_id) {
            append_lit(&b, ",\"tool_call_id\":");
            append_json_string(&b, messages[i].tool_call_id, messages[i].tool_call_id_len);
        }

        if (messages[i].name) {
            append_lit(&b, ",\"name\":");
            append_json_string(&b, messages[i].name, messages[i].name_len);
        }

        append_char(&b, '}');
    }
    append_char(&b, ']');

    if (stream) {
        append_lit(&b, ",\"stream\":true");
        if (include_usage) {
            append_lit(&b, ",\"stream_options\":{\"include_usage\":true}");
        }
    }

    if (params_json) {
        // Strip outer braces if present? OpenAI expects params at top level.
        // Usually params_json would be something like "\"temperature\": 0.7"
        // But if it's a full object "{\"temperature\": 0.7}", we need to merge it.
        // For simplicity, we assume params_json is either empty or a list of "key":value pairs.
        // Wait, the API says "params_json (optional JSON string)".
        // If it's a full object, we should probably strip braces.
        size_t p_len = strlen(params_json);
        if (p_len > 2 && params_json[0] == '{' && params_json[p_len - 1] == '}') {
            append_char(&b, ',');
            growbuf_append(&b, params_json + 1, p_len - 2, 0);
        } else if (p_len > 0) {
            append_char(&b, ',');
            growbuf_append(&b, params_json, p_len, 0);
        }
    }

    if (tooling_json) {
        size_t t_len = strlen(tooling_json);
        if (t_len > 2 && tooling_json[0] == '{' && tooling_json[t_len - 1] == '}') {
            append_char(&b, ',');
            growbuf_append(&b, tooling_json + 1, t_len - 2, 0);
        } else if (t_len > 0) {
            append_char(&b, ',');
            growbuf_append(&b, tooling_json, t_len, 0);
        }
    }

    if (response_format_json) {
        append_lit(&b, ",\"response_format\":");
        append_json_raw(&b, response_format_json, strlen(response_format_json));
    }

    append_char(&b, '}');
    append_char(&b, '\0');

    if (b.nomem) {
        growbuf_free(&b);
        return NULL;
    }

    return b.data;
}

char* build_completions_request(const char* model, const char* prompt, size_t prompt_len, bool stream,
                                bool include_usage, const char* params_json) {
    struct growbuf b;
    growbuf_init(&b, 4096);

    append_lit(&b, "{\"model\":");
    append_json_string(&b, model, strlen(model));
    append_lit(&b, ",\"prompt\":");
    append_json_string(&b, prompt, prompt_len);

    if (stream) {
        append_lit(&b, ",\"stream\":true");
        if (include_usage) {
            append_lit(&b, ",\"stream_options\":{\"include_usage\":true}");
        }
    }

    if (params_json) {
        size_t p_len = strlen(params_json);
        if (p_len > 2 && params_json[0] == '{' && params_json[p_len - 1] == '}') {
            append_char(&b, ',');
            growbuf_append(&b, params_json + 1, p_len - 2, 0);
        } else if (p_len > 0) {
            append_char(&b, ',');
            growbuf_append(&b, params_json, p_len, 0);
        }
    }

    append_char(&b, '}');
    append_char(&b, '\0');

    if (b.nomem) {
        growbuf_free(&b);
        return NULL;
    }

    return b.data;
}

char* build_embeddings_request(const char* model, const llm_embedding_input_t* inputs, size_t inputs_count,
                               const char* params_json, size_t max_input_bytes, size_t max_inputs) {
    if (!model) return NULL;
    if (inputs_count == 0) return NULL;
    if (inputs_count > 0 && !inputs) return NULL;
    if (max_inputs && inputs_count > max_inputs) return NULL;
    for (size_t i = 0; i < inputs_count; i++) {
        if (!inputs[i].text) return NULL;
        if (max_input_bytes && inputs[i].text_len > max_input_bytes) return NULL;
    }

    struct growbuf b;
    growbuf_init(&b, 4096);

    append_lit(&b, "{\"model\":");
    append_json_string(&b, model, strlen(model));
    append_lit(&b, ",\"input\":[");
    for (size_t i = 0; i < inputs_count; i++) {
        if (i > 0) append_char(&b, ',');
        append_json_string(&b, inputs[i].text, inputs[i].text_len);
    }
    append_char(&b, ']');

    if (params_json) {
        size_t p_len = strlen(params_json);
        if (p_len > 2 && params_json[0] == '{' && params_json[p_len - 1] == '}') {
            append_char(&b, ',');
            growbuf_append(&b, params_json + 1, p_len - 2, 0);
        } else if (p_len > 0) {
            append_char(&b, ',');
            growbuf_append(&b, params_json, p_len, 0);
        }
    }

    append_char(&b, '}');
    append_char(&b, '\0');

    if (b.nomem) {
        growbuf_free(&b);
        return NULL;
    }

    return b.data;
}
