#ifndef TOOLS_ACCUM_H
#define TOOLS_ACCUM_H

#include <stdbool.h>

#include "llm/internal.h"
#include "llm/llm.h"

struct tool_call_accumulator {
    char* id;
    char* name;
    struct growbuf args_buf;
    bool frozen;
};

void accum_init(struct tool_call_accumulator* acc);
void accum_free(struct tool_call_accumulator* acc);
bool accum_feed_delta(struct tool_call_accumulator* acc, const llm_tool_call_delta_t* delta, size_t max_args_bytes);
void accum_freeze(struct tool_call_accumulator* acc);

#endif  // TOOLS_ACCUM_H
