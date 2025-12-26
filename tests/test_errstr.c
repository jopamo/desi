#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "llm/llm.h"

static bool require(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return false;
    }
    return true;
}

static bool test_errstr_values(void) {
    const char* none = llm_errstr(LLM_ERR_NONE);
    if (!require(none && strcmp(none, "none") == 0, "errstr none")) return false;

    const char* cancelled = llm_errstr(LLM_ERR_CANCELLED);
    if (!require(cancelled && strcmp(cancelled, "cancelled") == 0, "errstr cancelled")) return false;

    const char* failed = llm_errstr(LLM_ERR_FAILED);
    if (!require(failed && strcmp(failed, "failed") == 0, "errstr failed")) return false;

    const char* unknown = llm_errstr((llm_error_t)99);
    if (!require(unknown && strcmp(unknown, "unknown") == 0, "errstr unknown")) return false;

    return true;
}

int main(void) {
    if (!test_errstr_values()) return 1;
    printf("Errstr tests passed.\n");
    return 0;
}
