#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm.h"

static void on_content(void* user_data, const char* delta, size_t len) {
    (void)user_data;
    printf("%.*s", (int)len, delta);
    fflush(stdout);
}

bool mock_dispatch(void* user_data, const char* tool_name, size_t name_len, const char* args_json, size_t args_len,
                   char** result_json, size_t* result_len) {
    (void)user_data;
    printf("Tool Dispatch: %.*s(%.*s)\n", (int)name_len, tool_name, (int)args_len, args_json);

    if (strncmp(tool_name, "get_weather", name_len) == 0) {
        const char* res = "{\"temperature\": 22, \"unit\": \"celsius\", \"description\": \"Sunny\"}";
        *result_json = strdup(res);
        *result_len = strlen(res);
        return true;
    }
    return false;
}

int main(void) {
    const char* env_url = getenv("LLM_BASE_URL");
    const char* base_url = env_url ? env_url : "http://10.0.5.69:8080";

    const char* env_model = getenv("LLM_MODEL");
    const char* default_model = "OpenAI-20B-NEO-CODEPlus16-Uncensored-IQ4_NL.gguf";
    const char* model_name = env_model ? env_model : default_model;

    printf("Connecting to %s with model %s\n", base_url, model_name);

    llm_model_t model;
    model.name = model_name;

    llm_client_t* client = llm_client_create(base_url, &model, NULL, NULL);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }

    printf("Checking health...\n");
    if (llm_health(client)) {
        printf("Health: OK\n");
    } else {
        fprintf(stderr, "Health check failed (Server likely down)\n");
        llm_client_destroy(client);
        return 1;
    }

    printf("Listing models...\n");
    size_t model_count = 0;
    char** models = llm_models_list(client, &model_count);
    if (models) {
        bool found = false;
        for (size_t i = 0; i < model_count; i++) {
            printf(" - %s\n", models[i]);
            if (strcmp(models[i], model_name) == 0) found = true;
        }
        llm_models_list_free(models, model_count);

        if (!found && !env_model) {
            printf("Warning: Default model '%s' not found in list.\n", model_name);
        }
    } else {
        fprintf(stderr, "Failed to list models\n");
    }

    printf("Testing chat (non-stream)...\n");
    llm_message_t messages[] = {{LLM_ROLE_USER, "What is 2+2?", 11, NULL, 0, NULL, 0}};

    llm_chat_result_t result;
    if (llm_chat(client, messages, 1, NULL, NULL, NULL, &result)) {
        printf("Assistant: %.*s\n", (int)result.content_len, result.content);
        llm_chat_result_free(&result);
    } else {
        fprintf(stderr, "Chat failed\n");
    }

    printf("Testing chat (stream)...\nAssistant: ");
    llm_stream_callbacks_t cbs = {0};
    cbs.on_content_delta = on_content;
    if (!llm_chat_stream(client, messages, 1, NULL, NULL, NULL, &cbs)) {
        fprintf(stderr, "Stream failed\n");
    }
    printf("\n");

    printf("Testing tool loop...\n");
    const char* tooling =
        "{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"description\":\"Get "
        "weather\",\"parameters\":{\"type\":\"object\",\"properties\":{\"location\":{\"type\":\"string\"}},"
        "\"required\":[\"location\"]}}}]}";
    llm_message_t tool_messages[] = {{LLM_ROLE_USER, "What is the weather in London?", 30, NULL, 0, NULL, 0}};

    if (llm_tool_loop_run(client, tool_messages, 1, tooling, mock_dispatch, NULL, 3)) {
        printf("Tool loop finished successfully\n");
    } else {
        fprintf(stderr, "Tool loop failed\n");
    }

    llm_client_destroy(client);
    return 0;
}
