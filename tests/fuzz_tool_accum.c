#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "src/tools_accum.h"

enum {
    FUZZ_MAX_INPUT = 4096,
    FUZZ_MAX_DELTAS = 64,
    FUZZ_MAX_ACCUMS = 4,
    FUZZ_MAX_ID = 64,
    FUZZ_MAX_NAME = 64,
    FUZZ_MAX_ARGS = 256
};

struct fuzz_cursor {
    const uint8_t* data;
    size_t len;
    size_t pos;
};

static uint8_t take_u8(struct fuzz_cursor* cur) {
    if (cur->pos >= cur->len) return 0;
    return cur->data[cur->pos++];
}

static void take_fragment(struct fuzz_cursor* cur, size_t max_len, const char** out, size_t* out_len) {
    if (cur->pos >= cur->len) {
        *out = NULL;
        *out_len = 0;
        return;
    }
    size_t want = (size_t)(take_u8(cur) % (max_len + 1));
    size_t remaining = cur->len - cur->pos;
    if (want > remaining) want = remaining;
    *out = (const char*)(cur->data + cur->pos);
    *out_len = want;
    cur->pos += want;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data) return 0;
    if (size > FUZZ_MAX_INPUT) size = FUZZ_MAX_INPUT;
    if (size == 0) return 0;

    struct fuzz_cursor cur = {.data = data, .len = size, .pos = 0};
    size_t acc_count = 1 + (take_u8(&cur) % FUZZ_MAX_ACCUMS);
    size_t max_args = (size_t)take_u8(&cur) << 8;
    max_args |= take_u8(&cur);
    max_args %= (FUZZ_MAX_ARGS + 1);
    size_t delta_count = take_u8(&cur) % FUZZ_MAX_DELTAS;

    struct tool_call_accumulator accums[FUZZ_MAX_ACCUMS];
    for (size_t i = 0; i < acc_count; i++) {
        accum_init(&accums[i]);
    }

    for (size_t i = 0; i < delta_count && cur.pos < cur.len; i++) {
        uint8_t flags = take_u8(&cur);
        size_t idx = take_u8(&cur) % acc_count;
        struct tool_call_accumulator* acc = &accums[idx];

        llm_tool_call_delta_t delta = {0};
        delta.index = idx;

        if (flags & 0x1) {
            take_fragment(&cur, FUZZ_MAX_ID, &delta.id, &delta.id_len);
        }
        if (flags & 0x2) {
            take_fragment(&cur, FUZZ_MAX_NAME, &delta.name, &delta.name_len);
        }
        if (flags & 0x4) {
            take_fragment(&cur, FUZZ_MAX_ARGS, &delta.arguments_fragment, &delta.arguments_fragment_len);
        }

        bool was_frozen = acc->frozen;
        size_t prev_len = acc->args_buf.len;
        char* prev_id = acc->id;
        char* prev_name = acc->name;

        bool ok = accum_feed_delta(acc, &delta, max_args);
        if (was_frozen) {
            if (ok) abort();
            if (acc->args_buf.len != prev_len) abort();
            if (acc->id != prev_id || acc->name != prev_name) abort();
        }
        if (max_args && acc->args_buf.len > max_args) abort();

        if (flags & 0x8) {
            accum_freeze(acc);
        }
        if (flags & 0x10) {
            accum_free(acc);
            accum_init(acc);
        }
    }

    for (size_t i = 0; i < acc_count; i++) {
        accum_free(&accums[i]);
    }
    return 0;
}
