#ifndef LLM_INTERNAL_H
#define LLM_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// String span (non-owning)
typedef struct {
    const char* ptr;
    size_t len;
} span_t;

static inline span_t span_from_cstr(const char* cstr) {
    span_t sp = {cstr, cstr ? strlen(cstr) : 0};
    return sp;
}

static inline bool span_eq(const span_t a, const span_t b) {
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

static inline bool span_eq_lit(const span_t a, const char* lit) {
    size_t lit_len = strlen(lit);
    return a.len == lit_len && memcmp(a.ptr, lit, lit_len) == 0;
}

// Growable buffer
struct growbuf {
    char* data;
    size_t len;
    size_t cap;
    bool nomem;
};

static inline void growbuf_init(struct growbuf* b, size_t initial_cap) {
    b->data = initial_cap ? malloc(initial_cap) : NULL;
    b->len = 0;
    b->cap = initial_cap;
    b->nomem = initial_cap && !b->data;
}

static inline bool growbuf_append(struct growbuf* b, const char* data, size_t len, size_t max_cap) {
    if (b->nomem) return false;
    if (len == 0) return true;
    if (max_cap && b->len + len > max_cap) return false;
    if (b->len + len > b->cap) {
        size_t next_cap = b->cap ? b->cap * 2 : 64;
        while (next_cap < b->len + len) next_cap *= 2;
        if (max_cap && next_cap > max_cap) {
            next_cap = max_cap;
        }
        char* new_data = realloc(b->data, next_cap);
        if (!new_data) {
            b->nomem = true;
            return false;
        }
        b->data = new_data;
        b->cap = next_cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return true;
}

static inline void growbuf_free(struct growbuf* b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

#endif  // LLM_INTERNAL_H