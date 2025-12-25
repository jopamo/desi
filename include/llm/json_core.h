#ifndef LLM_JSON_CORE_H
#define LLM_JSON_CORE_H

#include "internal.h"
#define JSTOK_HEADER
#include <jstok.h>

#ifdef __cplusplus
extern "C" {
#endif

// Helper: get span from token
span_t tok_span(const char* json, const jstoktok_t* tok);

// Helper: compare token with literal
bool tok_eq_lit(const char* json, const jstoktok_t* tok, const char* lit);

// Helper: token type check
bool tok_is_type(const jstoktok_t* tok, jstoktype_t type);

// Linear scan object for key, returns token index or -1
int obj_get_key(const jstoktok_t* tokens, int count, int obj_idx, const char* json, const char* key);

// Get array element, returns token index or -1
int arr_get(const jstoktok_t* tokens, int count, int arr_idx, int i);

// Skip subtree
int skip_subtree(const jstoktok_t* tokens, int count, int idx);

#ifdef __cplusplus
}
#endif

#endif  // LLM_JSON_CORE_H