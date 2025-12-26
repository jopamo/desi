#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm.h"

int main(void) {
    llm_model_t model = {"gpt-4o"};
    llm_client_t* client = llm_client_create("https://api.openai.com", &model, NULL, NULL);
    if (!client) return 1;

    llm_message_t messages[] = {{LLM_ROLE_USER, "Say hello!", 10, NULL, 0, NULL, 0, NULL, 0, NULL, 0}};

    llm_chat_result_t result;
    if (llm_chat(client, messages, 1, NULL, NULL, NULL, &result)) {
        if (result.content) {
            printf("Assistant: %.*s\n", (int)result.content_len, result.content);
        }
        llm_chat_result_free(&result);
    } else {
        fprintf(stderr, "Chat failed\n");
    }

    llm_client_destroy(client);
    return 0;
}
