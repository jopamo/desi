#include "tools_accum.h"

#include <stdlib.h>
#include <string.h>

void accum_init(struct tool_call_accumulator* acc) {
    acc->id = NULL;
    acc->name = NULL;
    growbuf_init(&acc->args_buf, 1024);
    acc->active = false;
    acc->saw_args = false;
    acc->frozen = false;
}

void accum_free(struct tool_call_accumulator* acc) {
    free(acc->id);
    free(acc->name);
    growbuf_free(&acc->args_buf);
}

static char* strdup_span(const char* ptr, size_t len) {
    if (!ptr) return NULL;
    char* s = malloc(len + 1);
    if (s) {
        memcpy(s, ptr, len);
        s[len] = '\0';
    }
    return s;
}

bool accum_feed_delta(struct tool_call_accumulator* acc, const llm_tool_call_delta_t* delta, size_t max_args_bytes) {
    if (acc->frozen) return false;
    acc->active = true;

    if (!acc->id && delta->id) {
        acc->id = strdup_span(delta->id, delta->id_len);
    }
    if (!acc->name && delta->name) {
        acc->name = strdup_span(delta->name, delta->name_len);
    }
    if (delta->arguments_fragment) {
        acc->saw_args = true;
        if (!growbuf_append(&acc->args_buf, delta->arguments_fragment, delta->arguments_fragment_len, max_args_bytes)) {
            return false;
        }
    }
    return true;
}

void accum_freeze(struct tool_call_accumulator* acc) {
    acc->frozen = true;
    // Ensure null termination of arguments for convenience
    if (growbuf_append(&acc->args_buf, "", 1, 0)) {
        acc->args_buf.len--;  // don't count null in length
    }
}
