#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm.h"

void on_content(void* user_data, const char* delta, size_t len) {
    (void)user_data;
    printf("%.*s", (int)len, delta);
    fflush(stdout);
}

void on_finish(void* user_data, llm_finish_reason_t reason) {
    (void)user_data;
    printf("\n[Finish reason: %s]\n", llm_finish_reason_to_string(reason));
}

int main(void) {
    llm_model_t model = {"gpt-4o"};
    llm_client_t* client = llm_client_create("https://api.openai.com", &model, NULL, NULL);
    if (!client) return 1;

    llm_message_t messages[] = {{LLM_ROLE_USER, "Tell me a short joke.", 21, NULL, 0, NULL, 0, NULL, 0}};

    llm_stream_callbacks_t cbs = {0};
    cbs.on_content_delta = on_content;
    cbs.on_finish_reason = on_finish;

    printf("Assistant: ");
    if (!llm_chat_stream(client, messages, 1, NULL, NULL, NULL, &cbs)) {
        fprintf(stderr, "\nStream failed\n");
    }

    llm_client_destroy(client);
    return 0;
}
