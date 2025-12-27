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

enum { SSE_MAX_EVENTS = 8, SSE_MAX_DATA = 256, SSE_MAX_TYPE = 64, SSE_MAX_ID = 64 };

struct sse_event_capture {
    char data[SSE_MAX_EVENTS][SSE_MAX_DATA];
    size_t data_len[SSE_MAX_EVENTS];
    char event_type[SSE_MAX_EVENTS][SSE_MAX_TYPE];
    size_t event_type_len[SSE_MAX_EVENTS];
    char last_id[SSE_MAX_EVENTS][SSE_MAX_ID];
    size_t last_id_len[SSE_MAX_EVENTS];
    size_t events;
    size_t frames;
};

static bool on_event_capture(void* user_data, const sse_event_t* event) {
    struct sse_event_capture* cap = user_data;
    if (cap->events >= SSE_MAX_EVENTS) return true;

    size_t idx = cap->events++;
    size_t copy_len = event->data.len;
    if (copy_len >= SSE_MAX_DATA) copy_len = SSE_MAX_DATA - 1;
    if (copy_len > 0 && event->data.ptr) {
        memcpy(cap->data[idx], event->data.ptr, copy_len);
    }
    cap->data_len[idx] = copy_len;
    cap->data[idx][copy_len] = '\0';

    size_t type_len = event->event_type.len;
    if (type_len >= SSE_MAX_TYPE) type_len = SSE_MAX_TYPE - 1;
    if (type_len > 0 && event->event_type.ptr) {
        memcpy(cap->event_type[idx], event->event_type.ptr, type_len);
    }
    cap->event_type_len[idx] = type_len;
    cap->event_type[idx][type_len] = '\0';

    size_t id_len = event->last_event_id.len;
    if (id_len >= SSE_MAX_ID) id_len = SSE_MAX_ID - 1;
    if (id_len > 0 && event->last_event_id.ptr) {
        memcpy(cap->last_id[idx], event->last_event_id.ptr, id_len);
    }
    cap->last_id_len[idx] = id_len;
    cap->last_id[idx][id_len] = '\0';

    return true;
}

static bool on_frame_count(void* user_data) {
    struct sse_event_capture* cap = user_data;
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

static bool test_sse_basic_event(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);
    sse_set_frame_callback(sse, on_frame_count, &cap);

    const char* payload = "data: {\"value\":\"ok\"}\n\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on valid frame")) return false;
    if (!require(cap.events == 1, "event not dispatched")) return false;
    if (!require(cap.frames == 1, "frame count")) return false;
    if (!require(cap.event_type_len[0] == 7 && memcmp(cap.event_type[0], "message", 7) == 0, "event type")) {
        return false;
    }
    span_t value = {0};
    if (!require(extract_string_field(cap.data[0], cap.data_len[0], "value", &value), "value field parse")) {
        return false;
    }
    if (!require(value.len == 2, "value length")) return false;
    if (!require(memcmp(value.ptr, "ok", 2) == 0, "value mismatch")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_comments_and_unknown(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    const char* payload =
        "event: ping\n"
        ": comment\n"
        "data {\"value\":\"skip\"}\n"
        "data: {\"value\":\"ok\"}\n\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on mixed payload")) return false;
    if (!require(cap.events == 1, "unexpected event count")) return false;
    if (!require(cap.event_type_len[0] == 4 && memcmp(cap.event_type[0], "ping", 4) == 0, "event type")) {
        return false;
    }

    span_t value = {0};
    if (!require(extract_string_field(cap.data[0], cap.data_len[0], "value", &value), "value field parse")) {
        return false;
    }
    if (!require(value.len == 2, "value length")) return false;
    if (!require(memcmp(value.ptr, "ok", 2) == 0, "value mismatch")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_line_endings(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    const char* payload = "data: one\r\ndata: two\rdata: three\n\r\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on line endings")) return false;
    if (!require(cap.events == 1, "event count")) return false;
    if (!require(cap.data_len[0] == 13, "data length")) return false;
    if (!require(memcmp(cap.data[0], "one\ntwo\nthree", 13) == 0, "data value")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_bom_strip(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    const char part1[] = {(char)0xEF, (char)0xBB};
    const char* part2 =
        "\xBF"
        "data: ok\n\n";
    int rc = sse_feed(sse, part1, sizeof(part1));
    if (!require(rc == SSE_OK, "sse_feed failed on bom part1")) return false;
    rc = sse_feed(sse, part2, strlen(part2));
    if (!require(rc == SSE_OK, "sse_feed failed on bom part2")) return false;
    if (!require(cap.events == 1, "event count")) return false;
    if (!require(cap.data_len[0] == 2, "data length")) return false;
    if (!require(memcmp(cap.data[0], "ok", 2) == 0, "data value")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_multiline_concat(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    const char* payload = "data: a\ndata: b\n\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on multiline")) return false;
    if (!require(cap.events == 1, "event count")) return false;
    if (!require(cap.data_len[0] == 3, "data length")) return false;
    if (!require(memcmp(cap.data[0], "a\nb", 3) == 0, "data value")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_event_only_no_data(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);
    sse_set_frame_callback(sse, on_frame_count, &cap);

    const char* payload = "event: ping\nid: 42\n\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on event-only")) return false;
    if (!require(cap.events == 0, "no data event dispatched")) return false;
    if (!require(cap.frames == 1, "frame count")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_incomplete_trailing_event(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    const char* payload = "data: hi\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on trailing event")) return false;
    if (!require(cap.events == 0, "incomplete trailing event dispatched")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_empty_data_lines(void) {
    sse_parser_t* sse = sse_create(16, 16, 128, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);
    sse_set_frame_callback(sse, on_frame_count, &cap);

    const char* payload =
        "data:\n"
        "data:\n"
        "\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on empty data lines")) return false;
    if (!require(cap.events == 1, "event count")) return false;
    if (!require(cap.data_len[0] == 1, "data length")) return false;
    if (!require(cap.data[0][0] == '\n', "data value")) return false;
    if (!require(cap.frames == 1, "frame count")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_crlf_chunk_boundary(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    int rc = sse_feed(sse, "data: ok\r", strlen("data: ok\r"));
    if (!require(rc == SSE_OK, "sse_feed failed on chunk1")) return false;
    if (!require(cap.events == 0, "event dispatched before CRLF complete")) return false;

    rc = sse_feed(sse, "\n\n", 2);
    if (!require(rc == SSE_OK, "sse_feed failed on chunk2")) return false;
    if (!require(cap.events == 1, "event count")) return false;
    if (!require(cap.data_len[0] == 2, "data length")) return false;
    if (!require(memcmp(cap.data[0], "ok", 2) == 0, "data value")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_id_nul_ignored(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    unsigned char payload[] = {
        'i', 'd',  ':', ' ', 'g',  'o', 'o', 'd', '\n', 'i', 'd', ':', ' ', 'b',  'a',
        'd', '\0', 'i', 'd', '\n', 'd', 'a', 't', 'a',  ':', ' ', 'o', 'k', '\n', '\n',
    };
    int rc = sse_feed(sse, (const char*)payload, sizeof(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on id payload")) return false;
    if (!require(cap.events == 1, "event count")) return false;
    if (!require(cap.last_id_len[0] == 4, "last id length")) return false;
    if (!require(memcmp(cap.last_id[0], "good", 4) == 0, "last id value")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_retry_digits_only(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;

    const char* payload = "retry: 100\nretry: bad\nretry: 42x\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on retry")) return false;
    size_t retry = 0;
    if (!require(sse_retry_ms(sse, &retry), "retry present")) return false;
    if (!require(retry == 100, "retry value")) return false;

    rc = sse_feed(sse, "retry: 250\n", strlen("retry: 250\n"));
    if (!require(rc == SSE_OK, "sse_feed failed on retry update")) return false;
    if (!require(sse_retry_ms(sse, &retry), "retry present after update")) return false;
    if (!require(retry == 250, "retry updated")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_utf8_replacement(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    const char* payload =
        "data: \xFF\n\n"
        "data: \xE2\n\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on invalid utf8")) return false;
    if (!require(cap.events == 2, "event count")) return false;
    const unsigned char repl[] = {0xEF, 0xBF, 0xBD};
    if (!require(cap.data_len[0] == sizeof(repl), "replacement length")) return false;
    if (!require(memcmp(cap.data[0], repl, sizeof(repl)) == 0, "replacement value 0")) return false;
    if (!require(cap.data_len[1] == sizeof(repl), "replacement length 1")) return false;
    if (!require(memcmp(cap.data[1], repl, sizeof(repl)) == 0, "replacement value 1")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_line_no_newline(void) {
    sse_parser_t* sse = sse_create(8, 0, 64, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;

    const char* payload = "data: 123456789";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_ERR_OVERFLOW_LINE, "expected line cap overflow")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_line_overflow_with_newline(void) {
    sse_parser_t* sse = sse_create(8, 0, 64, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;

    const char* payload = "data: 123456789\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_ERR_OVERFLOW_LINE, "expected line cap overflow with newline")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_partial_line_atomicity(void) {
    sse_parser_t* sse = sse_create(64, 64, 128, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    const char* chunk1 = "data: {\"value\":\"ok\"";
    int rc = sse_feed(sse, chunk1, strlen(chunk1));
    if (!require(rc == SSE_OK, "partial line chunk failed")) return false;
    if (!require(cap.events == 0, "event should not dispatch before newline")) return false;

    const char* chunk2 = "}\n\n";
    rc = sse_feed(sse, chunk2, strlen(chunk2));
    if (!require(rc == SSE_OK, "final line chunk failed")) return false;
    if (!require(cap.events == 1, "event not dispatched")) return false;

    span_t value = {0};
    if (!require(extract_string_field(cap.data[0], cap.data_len[0], "value", &value), "value field parse")) {
        return false;
    }
    if (!require(value.len == 2, "value length")) return false;
    if (!require(memcmp(value.ptr, "ok", 2) == 0, "value mismatch")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_partial_utf8_boundary(void) {
    sse_parser_t* sse = sse_create(128, 128, 256, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    const char chunk1[] =
        "data: {\"value\":\""
        "\xE2";
    const char chunk2[] = "\x82\xAC\"}\n\n";
    int rc = sse_feed(sse, chunk1, sizeof(chunk1) - 1);
    if (!require(rc == SSE_OK, "utf8 chunk1 failed")) return false;
    if (!require(cap.events == 0, "event should not dispatch on partial utf8")) return false;

    rc = sse_feed(sse, chunk2, sizeof(chunk2) - 1);
    if (!require(rc == SSE_OK, "utf8 chunk2 failed")) return false;
    if (!require(cap.events == 1, "event not dispatched")) return false;

    span_t value = {0};
    if (!require(extract_string_field(cap.data[0], cap.data_len[0], "value", &value), "value field parse")) {
        return false;
    }
    if (!require(value.len == 3, "utf8 value length")) return false;
    const unsigned char expected[] = {0xE2, 0x82, 0xAC};
    if (!require(memcmp(value.ptr, expected, sizeof(expected)) == 0, "utf8 value mismatch")) return false;

    sse_destroy(sse);
    return true;
}

static bool test_sse_non_json_payload(void) {
    sse_parser_t* sse = sse_create(64, 64, 128, 0);
    if (!require(sse != NULL, "sse_create failed")) return false;
    struct sse_event_capture cap = {0};
    sse_set_callback(sse, on_event_capture, &cap);

    const char* payload = "data: not-json\n\n";
    int rc = sse_feed(sse, payload, strlen(payload));
    if (!require(rc == SSE_OK, "sse_feed failed on non-json payload")) return false;
    if (!require(cap.events == 1, "event not dispatched")) return false;
    span_t value = {0};
    if (!require(!extract_string_field(cap.data[0], cap.data_len[0], "value", &value), "expected parse failure")) {
        return false;
    }

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
    if (!test_sse_basic_event()) return 1;
    if (!test_sse_comments_and_unknown()) return 1;
    if (!test_sse_line_endings()) return 1;
    if (!test_sse_bom_strip()) return 1;
    if (!test_sse_multiline_concat()) return 1;
    if (!test_sse_event_only_no_data()) return 1;
    if (!test_sse_incomplete_trailing_event()) return 1;
    if (!test_sse_empty_data_lines()) return 1;
    if (!test_sse_crlf_chunk_boundary()) return 1;
    if (!test_sse_id_nul_ignored()) return 1;
    if (!test_sse_retry_digits_only()) return 1;
    if (!test_sse_utf8_replacement()) return 1;
    if (!test_sse_line_no_newline()) return 1;
    if (!test_sse_line_overflow_with_newline()) return 1;
    if (!test_sse_partial_line_atomicity()) return 1;
    if (!test_sse_partial_utf8_boundary()) return 1;
    if (!test_sse_non_json_payload()) return 1;
    if (!test_sse_line_partial_chunks()) return 1;
    if (!test_sse_frame_overflow()) return 1;
    if (!test_sse_buffer_overflow()) return 1;
    if (!test_sse_total_overflow()) return 1;
    printf("SSE limits tests passed.\n");
    return 0;
}
