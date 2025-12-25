#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/tools_accum.h"

int main(void) {
    struct tool_call_accumulator acc;
    accum_init(&acc);

    const char* f1 = "{\"loc";
    llm_tool_call_delta_t d1 = {0, "call_123", 8, "get_weather", 11, f1, strlen(f1)};
    if (!accum_feed_delta(&acc, &d1, 1024)) return 1;

    const char* f2 = "ation\": \"London\"}";
    llm_tool_call_delta_t d2 = {0, NULL, 0, NULL, 0, f2, strlen(f2)};
    if (!accum_feed_delta(&acc, &d2, 1024)) return 1;

    accum_freeze(&acc);

    printf("ID: %s\n", acc.id);
    printf("Name: %s\n", acc.name);
    printf("Args: %s\n", acc.args_buf.data);

    if (strcmp(acc.id, "call_123") != 0) return 1;
    if (strcmp(acc.name, "get_weather") != 0) return 1;
    if (strcmp(acc.args_buf.data, "{\"location\": \"London\"}") != 0) return 1;

    accum_free(&acc);

    // Test cap
    accum_init(&acc);
    llm_tool_call_delta_t d3 = {0, "id", 2, "name", 4, "too long", 8};
    if (accum_feed_delta(&acc, &d3, 5)) {
        fprintf(stderr, "Cap should have been enforced\n");
        accum_free(&acc);
        return 1;
    }
    accum_free(&acc);

    printf("Tool accum test passed!\n");
    return 0;
}
