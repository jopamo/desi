#include "llm/json_core.h"

#include "llm/internal.h"
#include "llm/llm.h"
#define JSTOK_HEADER
#include <jstok.h>
#include <string.h>

// Convert jstok_span_t to span_t
static inline span_t span_from_jstok_span(jstok_span_t js) {
    span_t sp = {js.p, js.n};
    return sp;
}

// Helper: get span from token
span_t tok_span(const char* json, const jstoktok_t* tok) {
    jstok_span_t js = jstok_span(json, tok);
    return span_from_jstok_span(js);
}

// Helper: compare token with literal
bool tok_eq_lit(const char* json, const jstoktok_t* tok, const char* lit) { return jstok_eq(json, tok, lit) == 0; }

// Helper: token type check
bool tok_is_type(const jstoktok_t* tok, jstoktype_t type) { return tok->type == type; }

// Linear scan object for key, returns token index or -1
int obj_get_key(const jstoktok_t* tokens, int count, int obj_idx, const char* json, const char* key) {
    return jstok_object_get(json, tokens, count, obj_idx, key);
}

// Get array element, returns token index or -1
int arr_get(const jstoktok_t* tokens, int count, int arr_idx, int i) {
    return jstok_array_at(tokens, count, arr_idx, i);
}

// Skip subtree
int skip_subtree(const jstoktok_t* tokens, int count, int idx) { return jstok_skip(tokens, count, idx); }

llm_finish_reason_t llm_finish_reason_from_string(const char* str, size_t len) {
    if (len == 4 && memcmp(str, "stop", 4) == 0) return LLM_FINISH_REASON_STOP;
    if (len == 6 && memcmp(str, "length", 6) == 0) return LLM_FINISH_REASON_LENGTH;
    if (len == 10 && memcmp(str, "tool_calls", 10) == 0) return LLM_FINISH_REASON_TOOL_CALLS;
    if (len == 14 && memcmp(str, "content_filter", 14) == 0) return LLM_FINISH_REASON_CONTENT_FILTER;
    return LLM_FINISH_REASON_UNKNOWN;
}

const char* llm_finish_reason_to_string(llm_finish_reason_t reason) {
    switch (reason) {
        case LLM_FINISH_REASON_STOP:
            return "stop";
        case LLM_FINISH_REASON_LENGTH:
            return "length";
        case LLM_FINISH_REASON_TOOL_CALLS:
            return "tool_calls";
        case LLM_FINISH_REASON_CONTENT_FILTER:
            return "content_filter";
        default:
            return "unknown";
    }
}