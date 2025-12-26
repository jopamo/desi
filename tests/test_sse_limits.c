#define _POSIX_C_SOURCE 200809L
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

struct sse_capture {
    char buf[128];
    size_t len;
    bool called;
};

static void on_data_line(void* user_data, span_t line) {
    struct sse_capture* cap = user_data;
    if (line.len >= sizeof(cap->buf)) return;
    memcpy(cap->buf, line.ptr, line.len);
    cap->len = line.len;
    cap->buf[cap->len] = '\0';
    cap->called = true;
}

static bool extract_string_field(const char* json, size_t len, const char* key, span_t* out) {
    bool ok = false;
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return false;
    jstoktok_t* tokens = malloc((size_t)needed * sizeof(*tokens));
    if (!tokens) return false;
    jstok_init(&parser);
    int parsed = jstok_parse(&parser, json, (int)len, tokens, needed);
    if (parsed <= 0) goto cleanup;
    if (tokens[0].type != JSTOK_OBJECT) goto cleanup;
    int key_idx = obj_get_key(tokens, parsed, 0, json, key);
    if (key_idx < 0 || tokens[key_idx].type != JSTOK_STRING) goto cleanup;
    *out = tok_span(json, &tokens[key_idx]);
    ok = true;

cleanup:
    free(tokens);
    return ok;
}

static bool test_sse_valid_frame(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 512);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_capture cap = {0};
    sse_set_callback(sse, on_data_line, &cap);

    const char* payload = "data: {\"value\":\"ok\"}\n\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on valid frame")) return false;
    if (!require(cap.called, "callback not invoked on valid frame")) return false;

    span_t value = {0};
    if (!require(extract_string_field(cap.buf, cap.len, "value", &value), "value field parse failed")) return false;
    if (!require(value.len == 2, "value length mismatch")) return false;
    if (!require(memcmp(value.ptr, "ok", 2) == 0, "value mismatch")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_line_no_newline(void) {
    sse_parser_t* sse = sse_create(8, 0, 64, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_capture cap = {0};
    sse_set_callback(sse, on_data_line, &cap);

    const char* payload = "data: 123456789";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_ERR_OVERFLOW_LINE, "expected line cap overflow")) return false;
    if (!require(!cap.called, "callback should not fire on line overflow")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_line_partial_chunks(void) {
    sse_parser_t* sse = sse_create(12, 0, 64, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;

    int rc = sse_feed(sse, "data: 1234", 10);
    if (!require(rc == SSE_OK, "partial line should succeed")) return false;
    rc = sse_feed(sse, "5678", 4);
    if (!require(rc == SSE_ERR_OVERFLOW_LINE, "expected overflow after partial chunks")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_frame_overflow(void) {
    sse_parser_t* sse = sse_create(64, 8, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;

    const char* payload =
        "data: 12345\n"
        "data: 67890\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_ERR_OVERFLOW_FRAME, "expected frame cap overflow")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_buffer_overflow(void) {
    sse_parser_t* sse = sse_create(0, 0, 8, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;

    const char* payload = "data: 123456789";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_ERR_OVERFLOW_BUFFER, "expected buffer cap overflow")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_total_overflow(void) {
    sse_parser_t* sse = sse_create(0, 0, 64, 8);
    if (!require(sse != NULL, "sse_create failed")) return false;

    const char* payload = "data: 123456789";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_ERR_OVERFLOW_TOTAL, "expected total cap overflow")) return false;

    sse_destroy(sse);
    return true;
}

int main(void) {
    if (!test_sse_valid_frame()) return 1;
    if (!test_sse_line_no_newline()) return 1;
    if (!test_sse_line_partial_chunks()) return 1;
    if (!test_sse_frame_overflow()) return 1;
    if (!test_sse_buffer_overflow()) return 1;
    if (!test_sse_total_overflow()) return 1;
    printf("SSE limits tests passed.\n");
    return 0;
}
