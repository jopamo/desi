#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm.h"

// Mock weather tool
static bool weather_dispatch(void* user_data, const char* tool_name, size_t name_len, const char* args_json,
                             size_t args_len, char** result_json, size_t* result_len) {
    (void)user_data;
    printf("[Tool] Invoked: %.*s with args: %.*s\n", (int)name_len, tool_name, (int)args_len, args_json);

    if (strncmp(tool_name, "get_weather", name_len) == 0) {
        // In a real app, parse args_json to get location
        const char* mock_response =
            "{\"temperature\": 22, \"unit\": \"celsius\", \"description\": \"Sunny intervals\"}";
        *result_json = strdup(mock_response);
        *result_len = strlen(mock_response);
        printf("[Tool] Returning: %s\n", mock_response);
        return true;
    }

    return false;
}

int main(void) {
    const char* url = getenv("LLM_BASE_URL");
    if (!url) url = "http://127.0.0.1:8080";

    const char* model_name = getenv("LLM_MODEL");
    if (!model_name) model_name = "gpt-3.5-turbo";

    printf("Connecting to %s using model %s\n", url, model_name);

    llm_model_t model = {0};
    model.name = model_name;

    llm_client_t* client = llm_client_create(url, &model, NULL, NULL);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }

    if (!llm_health(client)) {
        fprintf(stderr, "Server health check failed at %s\n", url);
        llm_client_destroy(client);
        return 1;
    }

    // Define tool schema
    const char* tools =
        "{\"tools\": [{"
        "\"type\": \"function\","
        "\"function\": {"
        "\"name\": \"get_weather\","
        "\"description\": \"Get current weather for a location\","
        "\"parameters\": {"
        "\"type\": \"object\","
        "\"properties\": {"
        "\"location\": {\"type\": \"string\", \"description\": \"City, e.g. London\"}"
        "},"
        "\"required\": [\"location\"]"
        "}"
        "}"
        "}]}";

    llm_message_t initial_msg[] = {{LLM_ROLE_USER, "What's the weather like in London today?", 34, NULL, 0, NULL, 0}};

    printf("User: %s\n", initial_msg[0].content);

    if (llm_tool_loop_run(client, initial_msg, 1, tools, weather_dispatch, NULL, 5)) {
        printf("Tool loop completed.\n");
    } else {
        fprintf(stderr, "Tool loop failed.\n");
    }

    llm_client_destroy(client);
    return 0;
}
