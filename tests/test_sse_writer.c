#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/json_core.h"
#include "sse.h"

static bool require(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return false;
    }
    return true;
}

enum { CAP_DATA = 512, CAP_TYPE = 64 };

struct sse_capture {
    char data[CAP_DATA];
    size_t data_len;
    char event_type[CAP_TYPE];
    size_t event_type_len;
    size_t events;
    size_t frames;
};

static bool on_event_capture(void* user_data, const sse_event_t* event) {
    struct sse_capture* cap = user_data;
    cap->events++;

    if (event->data.len >= CAP_DATA) return false;
    if (event->data.len > 0 && event->data.ptr) {
        memcpy(cap->data, event->data.ptr, event->data.len);
    }
    cap->data_len = event->data.len;
    cap->data[cap->data_len] = '\0';

    if (event->event_type.len >= CAP_TYPE) return false;
    if (event->event_type.len > 0 && event->event_type.ptr) {
        memcpy(cap->event_type, event->event_type.ptr, event->event_type.len);
    }
    cap->event_type_len = event->event_type.len;
    cap->event_type[cap->event_type_len] = '\0';

    return true;
}

static bool on_frame_capture(void* user_data) {
    struct sse_capture* cap = user_data;
    cap->frames++;
    return true;
}

static bool parse_json_tokens(const char* json, size_t len, jstoktok_t** tokens_out, int* count_out) {
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return false;
    jstoktok_t* tokens = malloc((size_t)needed * sizeof(*tokens));
    if (!tokens) return false;
    jstok_init(&parser);
    int parsed = jstok_parse(&parser, json, (int)len, tokens, needed);
    if (parsed <= 0) {
        free(tokens);
        return false;
    }
    *tokens_out = tokens;
    *count_out = parsed;
    return true;
}

static bool json_expect_string(const char* json, const jstoktok_t* tokens, int count, int obj_idx, const char* key,
                               const char* expected) {
    int idx = jstok_object_get(json, tokens, count, obj_idx, key);
    if (idx < 0 || tokens[idx].type != JSTOK_STRING) return false;
    jstok_span_t sp = jstok_span(json, &tokens[idx]);
    size_t expected_len = strlen(expected);
    return sp.n == expected_len && memcmp(sp.p, expected, expected_len) == 0;
}

static bool json_expect_number(const char* json, const jstoktok_t* tokens, int count, int obj_idx, const char* key,
                               long long expected) {
    int idx = jstok_object_get(json, tokens, count, obj_idx, key);
    if (idx < 0 || tokens[idx].type != JSTOK_PRIMITIVE) return false;
    long long value = 0;
    if (jstok_atoi64(json, &tokens[idx], &value) != 0) return false;
    return value == expected;
}

static bool feed_payload(sse_parser_t* sse, const char* payload, size_t len, size_t chunk) {
    size_t pos = 0;
    if (chunk == 0) chunk = len;
    while (pos < len) {
        size_t cur = chunk;
        if (cur > len - pos) cur = len - pos;
        int rc = sse_feed(sse, payload + pos, cur);
        if (rc != SSE_OK) return false;
        pos += cur;
    }
    return true;
}

static bool test_sse_writer_roundtrip_basic(void) {
    sse_write_limits_t limits = {.max_line_bytes = 64, .max_frame_bytes = 256};
    const char* event_type = "update";
    const char* data = " {\n\"value\":\"ok\",\n\"count\":2\n}";

    char out[512];
    size_t out_len = 0;
    int rc = sse_write_event(&limits, event_type, strlen(event_type), data, strlen(data), out, sizeof(out), &out_len);
    if (!require(rc == SSE_OK, "writer basic")) return false;

    sse_parser_t* sse = sse_create(limits.max_line_bytes, limits.max_frame_bytes, 0, 0);
    if (!require(sse != NULL, "sse_create")) return false;

    struct sse_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);
    sse_set_frame_callback(sse, on_frame_capture, &cap);

    if (!require(feed_payload(sse, out, out_len, 0), "sse_feed")) {
        sse_destroy(sse);
        return false;
    }

    if (!require(cap.events == 1, "event count")) {
        sse_destroy(sse);
        return false;
    }
    if (!require(cap.event_type_len == strlen(event_type), "event type len")) {
        sse_destroy(sse);
        return false;
    }
    if (!require(memcmp(cap.event_type, event_type, cap.event_type_len) == 0, "event type")) {
        sse_destroy(sse);
        return false;
    }
    if (!require(cap.data_len == strlen(data), "data len")) {
        sse_destroy(sse);
        return false;
    }
    if (!require(memcmp(cap.data, data, cap.data_len) == 0, "data bytes")) {
        sse_destroy(sse);
        return false;
    }
    if (!require(cap.data_len > 0 && cap.data[0] == ' ', "leading space")) {
        sse_destroy(sse);
        return false;
    }

    jstoktok_t* tokens = NULL;
    int count = 0;
    if (!require(parse_json_tokens(cap.data, cap.data_len, &tokens, &count), "parse json")) {
        sse_destroy(sse);
        return false;
    }
    bool ok = true;
    ok = ok && require(tokens[0].type == JSTOK_OBJECT, "json object");
    ok = ok && require(json_expect_string(cap.data, tokens, count, 0, "value", "ok"), "json value");
    ok = ok && require(json_expect_number(cap.data, tokens, count, 0, "count", 2), "json count");
    free(tokens);
    sse_destroy(sse);
    return ok;
}

static bool test_sse_writer_roundtrip_streamed(void) {
    sse_write_limits_t limits = {.max_line_bytes = 64, .max_frame_bytes = 128};
    const char* data = "{\"value\":\"ok\"}";

    char out[256];
    size_t out_len = 0;
    int rc = sse_write_event(&limits, NULL, 0, data, strlen(data), out, sizeof(out), &out_len);
    if (!require(rc == SSE_OK, "writer stream")) return false;

    sse_parser_t* sse = sse_create(limits.max_line_bytes, limits.max_frame_bytes, 0, 0);
    if (!require(sse != NULL, "sse_create")) return false;

    struct sse_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    if (!require(feed_payload(sse, out, out_len, 3), "sse_feed chunked")) {
        sse_destroy(sse);
        return false;
    }

    if (!require(cap.events == 1, "stream event count")) {
        sse_destroy(sse);
        return false;
    }
    if (!require(cap.event_type_len == strlen("message"), "default event len")) {
        sse_destroy(sse);
        return false;
    }
    if (!require(memcmp(cap.event_type, "message", cap.event_type_len) == 0, "default event type")) {
        sse_destroy(sse);
        return false;
    }

    jstoktok_t* tokens = NULL;
    int count = 0;
    if (!require(parse_json_tokens(cap.data, cap.data_len, &tokens, &count), "parse stream json")) {
        sse_destroy(sse);
        return false;
    }
    bool ok = true;
    ok = ok && require(tokens[0].type == JSTOK_OBJECT, "stream json object");
    ok = ok && require(json_expect_string(cap.data, tokens, count, 0, "value", "ok"), "stream json value");
    free(tokens);
    sse_destroy(sse);
    return ok;
}

static bool test_sse_writer_keepalive(void) {
    sse_write_limits_t limits = {.max_line_bytes = 16, .max_frame_bytes = 0};
    char out[32];
    size_t out_len = 0;
    int rc = sse_write_keepalive(&limits, out, sizeof(out), &out_len);
    if (!require(rc == SSE_OK, "keepalive write")) return false;

    sse_parser_t* sse = sse_create(32, 32, 0, 0);
    if (!require(sse != NULL, "sse_create")) return false;

    struct sse_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);
    sse_set_frame_callback(sse, on_frame_capture, &cap);

    if (!require(feed_payload(sse, out, out_len, 0), "keepalive feed")) {
        sse_destroy(sse);
        return false;
    }
    if (!require(cap.events == 0, "keepalive events")) {
        sse_destroy(sse);
        return false;
    }
    if (!require(cap.frames == 1, "keepalive frames")) {
        sse_destroy(sse);
        return false;
    }

    sse_destroy(sse);
    return true;
}

static bool test_sse_writer_limits(void) {
    char out[64];
    size_t out_len = 0;

    sse_write_limits_t limits = {.max_line_bytes = 8, .max_frame_bytes = 32};
    const char* data = "{\"v\":1}";
    int rc = sse_write_event(&limits, "toolong", 7, data, strlen(data), out, sizeof(out), &out_len);
    if (!require(rc == SSE_ERR_OVERFLOW_LINE, "event line overflow")) return false;

    limits.max_line_bytes = 64;
    limits.max_frame_bytes = 4;
    rc = sse_write_event(&limits, NULL, 0, "1234", 4, out, sizeof(out), &out_len);
    if (!require(rc == SSE_ERR_OVERFLOW_FRAME, "frame overflow")) return false;

    limits.max_line_bytes = 6;
    limits.max_frame_bytes = 0;
    rc = sse_write_event(&limits, NULL, 0, "a", 1, out, sizeof(out), &out_len);
    if (!require(rc == SSE_ERR_OVERFLOW_LINE, "data line overflow")) return false;

    limits.max_line_bytes = 64;
    limits.max_frame_bytes = 64;
    rc = sse_write_event(&limits, NULL, 0, "abcd", 4, out, 4, &out_len);
    if (!require(rc == SSE_ERR_OVERFLOW_BUFFER, "buffer overflow")) return false;

    rc = sse_write_event(&limits, "bad\n", 4, data, strlen(data), out, sizeof(out), &out_len);
    if (!require(rc == SSE_ERR_BAD_INPUT, "bad event input")) return false;

    rc = sse_write_event(&limits, NULL, 0, "a\r", 2, out, sizeof(out), &out_len);
    if (!require(rc == SSE_ERR_BAD_INPUT, "bad data input")) return false;

    return true;
}

int main(void) {
    if (!test_sse_writer_roundtrip_basic()) return 1;
    if (!test_sse_writer_roundtrip_streamed()) return 1;
    if (!test_sse_writer_keepalive()) return 1;
    if (!test_sse_writer_limits()) return 1;
    printf("SSE writer tests passed.\n");
    return 0;
}
