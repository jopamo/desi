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

enum { SSE_MULTI_MAX_LINES = 8, SSE_MULTI_MAX_LINE = 128 };

struct sse_multi_capture {
    char lines[SSE_MULTI_MAX_LINES][SSE_MULTI_MAX_LINE];
    size_t lens[SSE_MULTI_MAX_LINES];
    size_t calls;
    size_t frames;
};

static void on_data_line(void* user_data, span_t line) {
    struct sse_capture* cap = user_data;
    if (line.len >= sizeof(cap->buf)) return;
    memcpy(cap->buf, line.ptr, line.len);
    cap->len = line.len;
    cap->buf[cap->len] = '\0';
    cap->called = true;
}

static void on_data_line_multi(void* user_data, span_t line) {
    struct sse_multi_capture* cap = user_data;
    if (cap->calls >= SSE_MULTI_MAX_LINES) return;
    size_t idx = cap->calls++;
    size_t copy_len = line.len;
    if (copy_len >= SSE_MULTI_MAX_LINE) {
        copy_len = SSE_MULTI_MAX_LINE - 1;
    }
    if (copy_len > 0) {
        memcpy(cap->lines[idx], line.ptr, copy_len);
    }
    cap->lens[idx] = copy_len;
    cap->lines[idx][copy_len] = '\0';
}

static bool on_frame_count(void* user_data) {
    struct sse_multi_capture* cap = user_data;
    cap->frames++;
    return true;
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

static bool test_sse_done_handling(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_multi_capture cap = {0};
    sse_set_callback(sse, on_data_line_multi, &cap);

    const char* payload =
        "data: {\"value\":\"ok\"}\n\n"
        "data: [DONE]\n\n"
        "data: {\"value\":\"late\"}\n\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on done payload")) return false;
    if (!require(sse_is_done(sse), "expected done state after [DONE]")) return false;
    if (!require(cap.calls == 1, "unexpected callbacks after [DONE]")) return false;

    span_t value = {0};
    if (!require(extract_string_field(cap.lines[0], cap.lens[0], "value", &value), "value field parse failed")) {
        return false;
    }
    if (!require(value.len == 2, "value length mismatch")) return false;
    if (!require(memcmp(value.ptr, "ok", 2) == 0, "value mismatch")) return false;

    const char* late = "data: {\"value\":\"after\"}\n\n";
    rc = sse_feed(sse, late, strlen(late));
    if (!require(rc == SSE_OK, "sse_feed should ignore after done")) return false;
    if (!require(cap.calls == 1, "callback should not run after done")) return false;

    sse_destroy(sse);
    return true;
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

static bool test_sse_malformed_lines_ignored(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_multi_capture cap = {0};
    sse_set_callback(sse, on_data_line_multi, &cap);

    const char* payload =
        "event: ping\n"
        ": comment\n"
        "data {\"value\":\"skip\"}\n"
        "data: {\"value\":\"ok\"}\n\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on mixed payload")) return false;
    if (!require(cap.calls == 1, "unexpected callbacks on malformed lines")) return false;
    if (!require(!sse_is_done(sse), "malformed lines should not set done")) return false;

    span_t value = {0};
    if (!require(extract_string_field(cap.lines[0], cap.lens[0], "value", &value), "value field parse failed")) {
        return false;
    }
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

static bool test_sse_line_overflow_with_newline(void) {
    sse_parser_t* sse = sse_create(8, 0, 64, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_capture cap = {0};
    sse_set_callback(sse, on_data_line, &cap);

    const char* payload = "data: 123456789\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_ERR_OVERFLOW_LINE, "expected line cap overflow with newline")) return false;
    if (!require(!cap.called, "callback should not fire on line overflow")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_partial_line_atomicity(void) {
    sse_parser_t* sse = sse_create(64, 64, 128, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_capture cap = {0};
    sse_set_callback(sse, on_data_line, &cap);

    const char* chunk1 = "data: {\"value\":\"ok\"";
    int rc = sse_feed(sse, chunk1, strlen(chunk1));
    if (!require(rc == SSE_OK, "partial line chunk failed")) return false;
    if (!require(!cap.called, "callback should not fire before newline")) return false;

    const char* chunk2 = "}\n\n";
    rc = sse_feed(sse, chunk2, strlen(chunk2));
    if (!require(rc == SSE_OK, "final line chunk failed")) return false;
    if (!require(cap.called, "callback not invoked after line completion")) return false;

    span_t value = {0};
    if (!require(extract_string_field(cap.buf, cap.len, "value", &value), "value field parse failed")) return false;
    if (!require(value.len == 2, "value length mismatch")) return false;
    if (!require(memcmp(value.ptr, "ok", 2) == 0, "value mismatch")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_partial_utf8_boundary(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_capture cap = {0};
    sse_set_callback(sse, on_data_line, &cap);

    const char chunk1[] =
        "data: {\"value\":\""
        "\xE2";
    const char chunk2[] = "\x82\xAC\"}\n\n";
    int rc = sse_feed(sse, chunk1, sizeof(chunk1) - 1);
    if (!require(rc == SSE_OK, "utf8 chunk1 failed")) return false;
    if (!require(!cap.called, "callback should not fire on partial utf8")) return false;

    rc = sse_feed(sse, chunk2, sizeof(chunk2) - 1);
    if (!require(rc == SSE_OK, "utf8 chunk2 failed")) return false;
    if (!require(cap.called, "callback not invoked after utf8 completion")) return false;

    span_t value = {0};
    if (!require(extract_string_field(cap.buf, cap.len, "value", &value), "value field parse failed")) return false;
    if (!require(value.len == 3, "utf8 value length mismatch")) return false;
    const unsigned char expected[] = {0xE2, 0x82, 0xAC};
    if (!require(memcmp(value.ptr, expected, sizeof(expected)) == 0, "utf8 value mismatch")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_empty_data_lines(void) {
    sse_parser_t* sse = sse_create(16, 16, 128, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_multi_capture cap = {0};
    sse_set_callback(sse, on_data_line_multi, &cap);
    sse_set_frame_callback(sse, on_frame_count, &cap);

    const char* payload =
        "data:\n"
        "data:\n"
        "\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on empty data lines")) return false;
    if (!require(cap.calls == 2, "expected callbacks for empty data lines")) return false;
    if (!require(cap.lens[0] == 0 && cap.lens[1] == 0, "expected empty payloads")) return false;
    if (!require(cap.frames == 1, "expected one frame boundary")) return false;
    if (!require(!sse_is_done(sse), "empty data lines should not set done")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_non_json_payload(void) {
    sse_parser_t* sse = sse_create(64, 64, 128, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_capture cap = {0};
    sse_set_callback(sse, on_data_line, &cap);

    const char* payload = "data: not-json\n\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on non-json payload")) return false;
    if (!require(cap.called, "callback not invoked on non-json payload")) return false;
    span_t value = {0};
    if (!require(!extract_string_field(cap.buf, cap.len, "value", &value), "expected non-json parse failure")) {
        return false;
    }
    if (!require(!sse_is_done(sse), "non-json payload should not set done")) return false;

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
    if (!test_sse_done_handling()) return 1;
    if (!test_sse_valid_frame()) return 1;
    if (!test_sse_malformed_lines_ignored()) return 1;
    if (!test_sse_line_no_newline()) return 1;
    if (!test_sse_line_overflow_with_newline()) return 1;
    if (!test_sse_partial_line_atomicity()) return 1;
    if (!test_sse_partial_utf8_boundary()) return 1;
    if (!test_sse_empty_data_lines()) return 1;
    if (!test_sse_non_json_payload()) return 1;
    if (!test_sse_line_partial_chunks()) return 1;
    if (!test_sse_frame_overflow()) return 1;
    if (!test_sse_buffer_overflow()) return 1;
    if (!test_sse_total_overflow()) return 1;
    printf("SSE limits tests passed.\n");
    return 0;
}
