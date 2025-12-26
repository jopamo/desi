#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/json_build.h"

int main(void) {
    llm_message_t messages[] = {{LLM_ROLE_SYSTEM, "You are a helpful assistant.",
                                 strlen("You are a helpful assistant."), NULL, 0, NULL, 0, NULL, 0},
                                {LLM_ROLE_USER, "Hello!", strlen("Hello!"), NULL, 0, NULL, 0, NULL, 0}};

    char* json = build_chat_request("gpt-4o", messages, 2, false, false, "{\"temperature\":0.7}", NULL, NULL);
    if (!json) {
        fprintf(stderr, "build_chat_request failed\n");
        return 1;
    }

    printf("Generated JSON: %s\n", json);

    // Basic validation
    if (!strstr(json, "\"model\":\"gpt-4o\"")) {
        fprintf(stderr, "FAIL: model\n");
        return 1;
    }
    if (!strstr(json, "\"role\":\"system\"")) {
        fprintf(stderr, "FAIL: role\n");
        return 1;
    }
    if (!strstr(json, "\"content\":\"Hello!\"")) {
        fprintf(stderr, "FAIL: content\n");
        return 1;
    }
    if (!strstr(json, "\"temperature\":0.7")) {
        fprintf(stderr, "FAIL: temperature\n");
        return 1;
    }

    free(json);

    // Test escaping
    const char* esc_content = "Quotes: \" and Backslash: \\";
    llm_message_t msg_esc[] = {{LLM_ROLE_USER, esc_content, strlen(esc_content), NULL, 0, NULL, 0, NULL, 0}};
    json = build_chat_request("gpt-4o", msg_esc, 1, false, false, NULL, NULL, NULL);
    printf("Escaped JSON: %s\n", json);
    if (!strstr(json, "Quotes: \\\" and Backslash: \\\\")) {
        fprintf(stderr, "FAIL: escaping\n");
        return 1;
    }
    free(json);

    printf("JSON build test passed!\n");
    return 0;
}
