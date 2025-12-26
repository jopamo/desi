#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "llm/llm.h"

#define JSTOK_HEADER
#include <jstok.h>

#define ASSERT(cond, msg)                       \
    do {                                        \
        if (!(cond)) {                          \
            fprintf(stderr, "FAIL: %s\n", msg); \
            return false;                       \
        }                                       \
    } while (0)

struct expected_headers {
    const char* auth;
    const char* org;
    const char* project;
    const char* custom;
};

struct stream_capture {
    char* data;
    size_t len;
    size_t cap;
    bool failed;
};

static void stream_capture_append(struct stream_capture* cap, const char* data, size_t len) {
    if (cap->failed) return;
    if (len == 0) return;
    size_t needed = cap->len + len + 1;
    if (needed > cap->cap) {
        size_t next_cap = cap->cap ? cap->cap * 2 : 128;
        while (next_cap < needed) next_cap *= 2;
        char* next = realloc(cap->data, next_cap);
        if (!next) {
            cap->failed = true;
            return;
        }
        cap->data = next;
        cap->cap = next_cap;
    }
    memcpy(cap->data + cap->len, data, len);
    cap->len += len;
    cap->data[cap->len] = '\0';
}

static void on_stream_content(void* user_data, const char* delta, size_t len) {
    struct stream_capture* cap = user_data;
    stream_capture_append(cap, delta, len);
}

static int create_listener(uint16_t* port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) != 0) {
        close(fd);
        return -1;
    }

    *port_out = ntohs(addr.sin_port);
    return fd;
}

static bool send_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static int parse_content_length(const char* buf, size_t header_len) {
    const char* key = "Content-Length:";
    size_t key_len = strlen(key);
    const char* cursor = buf;
    const char* end = buf + header_len;

    while (cursor < end) {
        const char* line_end = strstr(cursor, "\r\n");
        if (!line_end || line_end > end) break;
        if ((size_t)(line_end - cursor) >= key_len && strncmp(cursor, key, key_len) == 0) {
            const char* val = cursor + key_len;
            while (val < line_end && (*val == ' ' || *val == '\t')) val++;
            return atoi(val);
        }
        cursor = line_end + 2;
    }

    return 0;
}

static bool header_value(const char* buf, size_t header_len, const char* key, char* out, size_t out_cap) {
    size_t key_len = strlen(key);
    const char* cursor = buf;
    const char* end = buf + header_len;

    while (cursor < end) {
        const char* line_end = strstr(cursor, "\r\n");
        if (!line_end || line_end > end) break;
        if ((size_t)(line_end - cursor) > key_len + 1 && strncmp(cursor, key, key_len) == 0 && cursor[key_len] == ':') {
            const char* val = cursor + key_len + 1;
            while (val < line_end && (*val == ' ' || *val == '\t')) val++;
            size_t len = (size_t)(line_end - val);
            if (len >= out_cap) len = out_cap - 1;
            memcpy(out, val, len);
            out[len] = '\0';
            return true;
        }
        cursor = line_end + 2;
    }

    return false;
}

static bool header_matches(const char* buf, size_t header_len, const char* key, const char* expected) {
    if (!expected) return true;
    char value[256];
    if (!header_value(buf, header_len, key, value, sizeof(value))) return false;
    return strcmp(value, expected) == 0;
}

static bool header_has_token(const char* buf, size_t header_len, const char* key, const char* token) {
    char value[256];
    if (!header_value(buf, header_len, key, value, sizeof(value))) return false;
    return strstr(value, token) != NULL;
}

static bool parse_request_line(const char* buf, char* method, size_t method_cap, char* path, size_t path_cap) {
    const char* line_end = strstr(buf, "\r\n");
    if (!line_end) return false;
    if (sscanf(buf, "%7s %255s", method, path) != 2) return false;
    method[method_cap - 1] = '\0';
    path[path_cap - 1] = '\0';
    return true;
}

static bool read_request(int fd, char* buf, size_t cap, size_t* header_len, size_t* body_len) {
    size_t used = 0;
    char* header_end = NULL;

    while (used + 1 < cap) {
        ssize_t n = recv(fd, buf + used, cap - 1 - used, 0);
        if (n <= 0) return false;
        used += (size_t)n;
        buf[used] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
        if (header_end) break;
    }

    if (!header_end) return false;

    *header_len = (size_t)(header_end - buf);
    int content_len = parse_content_length(buf, *header_len);

    if (header_has_token(buf, *header_len, "Expect", "100-continue")) {
        if (!send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25)) return false;
    }

    size_t total_needed = *header_len + 4 + (size_t)content_len;
    while (used < total_needed && used + 1 < cap) {
        ssize_t n = recv(fd, buf + used, cap - 1 - used, 0);
        if (n <= 0) return false;
        used += (size_t)n;
    }

    if (used < total_needed) return false;

    buf[total_needed] = '\0';
    *body_len = (size_t)content_len;
    return true;
}

static bool send_response_status(int fd, const char* status, const char* content_type, const char* body,
                                 size_t body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              status, content_type, body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return false;
    if (!send_all(fd, header, (size_t)header_len)) return false;
    return send_all(fd, body, body_len);
}

static bool send_response(int fd, const char* content_type, const char* body, size_t body_len) {
    return send_response_status(fd, "200 OK", content_type, body, body_len);
}

static bool send_not_found(int fd) {
    const char* body = "{\"error\":\"not found\"}";
    return send_response(fd, "application/json", body, strlen(body));
}

static bool respond_auth_error(int fd) {
    const char* body =
        "{\"error\":{\"message\":\"missing api key\",\"type\":\"auth_error\",\"code\":\"missing_api_key\"}}";
    return send_response_status(fd, "401 Unauthorized", "application/json", body, strlen(body));
}

static bool respond_props(int fd, bool auth_ok, bool org_ok, bool project_ok, bool custom_ok) {
    const char* auth = auth_ok ? "true" : "false";
    const char* org = org_ok ? "true" : "false";
    const char* project = project_ok ? "true" : "false";
    const char* custom = custom_ok ? "true" : "false";
    char body[256];
    int len = snprintf(body, sizeof(body), "{\"auth\":%s,\"org\":%s,\"project\":%s,\"custom\":%s,\"path\":\"/props\"}",
                       auth, org, project, custom);
    if (len < 0 || (size_t)len >= sizeof(body)) return false;
    return send_response(fd, "application/json", body, (size_t)len);
}

static bool respond_chat(int fd, bool auth_ok, bool org_ok, bool project_ok, bool custom_ok) {
    const char* auth = auth_ok ? "true" : "false";
    const char* org = org_ok ? "true" : "false";
    const char* project = project_ok ? "true" : "false";
    const char* custom = custom_ok ? "true" : "false";
    char body[768];
    int len = snprintf(body, sizeof(body),
                       "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{"
                       "\"content\":\"{\\\"auth\\\":%s,\\\"org\\\":%s,\\\"project\\\":%s,\\\"custom\\\":%s,"
                       "\\\"mode\\\":\\\"chat\\\"}\","
                       "\"tool_calls\":[{\"id\":\"call_1\",\"function\":{\"name\":\"ping\","
                       "\"arguments\":\"{\\\"echo\\\":\\\"ok\\\",\\\"auth\\\":%s,\\\"org\\\":%s,\\\"project\\\":%s,"
                       "\\\"custom\\\":%s}\"}}]}}]}",
                       auth, org, project, custom, auth, org, project, custom);
    if (len < 0 || (size_t)len >= sizeof(body)) return false;
    return send_response(fd, "application/json", body, (size_t)len);
}

static bool respond_chat_stream(int fd, bool auth_ok, bool org_ok, bool project_ok, bool custom_ok) {
    const char* auth = auth_ok ? "true" : "false";
    const char* org = org_ok ? "true" : "false";
    const char* project = project_ok ? "true" : "false";
    const char* custom = custom_ok ? "true" : "false";
    char body[640];
    int len = snprintf(body, sizeof(body),
                       "data: {\"choices\":[{\"delta\":{\"content\":\"{\\\"auth\\\":%s,\\\"org\\\":%s,"
                       "\\\"project\\\":%s,\\\"custom\\\":%s,\\\"mode\\\":\\\"stream\\\"}\"}}]}\n\n"
                       "data: [DONE]\n\n",
                       auth, org, project, custom);
    if (len < 0 || (size_t)len >= sizeof(body)) return false;
    return send_response(fd, "text/event-stream", body, (size_t)len);
}

static bool handle_request(int fd, const struct expected_headers* expected) {
    char buf[8192];
    size_t header_len = 0;
    size_t body_len = 0;

    if (!read_request(fd, buf, sizeof(buf), &header_len, &body_len)) return false;

    char method[8] = {0};
    char path[256] = {0};
    if (!parse_request_line(buf, method, sizeof(method), path, sizeof(path))) return false;

    bool auth_ok = header_matches(buf, header_len, "Authorization", expected->auth);
    bool org_ok = header_matches(buf, header_len, "OpenAI-Organization", expected->org);
    bool project_ok = header_matches(buf, header_len, "OpenAI-Project", expected->project);
    bool custom_ok = header_matches(buf, header_len, "X-Custom-Header", expected->custom);

    if (!auth_ok) {
        return respond_auth_error(fd);
    }

    const char* body = buf + header_len + 4;
    bool is_stream = false;
    if (body_len > 0 && strstr(body, "\"stream\":true")) {
        is_stream = true;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/props") == 0) {
        return respond_props(fd, auth_ok, org_ok, project_ok, custom_ok);
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/chat/completions") == 0) {
        if (is_stream) {
            return respond_chat_stream(fd, auth_ok, org_ok, project_ok, custom_ok);
        }
        return respond_chat(fd, auth_ok, org_ok, project_ok, custom_ok);
    }

    return send_not_found(fd);
}

static void server_loop(int listener, const struct expected_headers* expected, int requests) {
    for (int i = 0; i < requests; i++) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) _exit(1);
        bool ok = handle_request(fd, &expected[i]);
        close(fd);
        if (!ok) _exit(1);
    }
    close(listener);
    _exit(0);
}

static bool parse_json_tokens(const char* json, size_t len, jstoktok_t** tokens_out, int* count_out) {
    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, json, (int)len, NULL, 0);
    if (needed <= 0) return false;

    jstoktok_t* tokens = malloc((size_t)needed * sizeof(jstoktok_t));
    if (!tokens) return false;

    jstok_init(&parser);
    int parsed = jstok_parse(&parser, json, (int)len, tokens, needed);
    if (parsed <= 0) {
        free(tokens);
        return false;
    }

    *tokens_out = tokens;
    *count_out = parsed;
    return true;
}

static bool json_expect_bool(const char* json, const jstoktok_t* tokens, int count, int obj_idx, const char* key,
                             bool expected) {
    int idx = jstok_object_get(json, tokens, count, obj_idx, key);
    if (idx < 0 || tokens[idx].type != JSTOK_PRIMITIVE) return false;
    int value = 0;
    if (jstok_atob(json, &tokens[idx], &value) != 0) return false;
    return (value != 0) == expected;
}

static bool json_expect_string(const char* json, const jstoktok_t* tokens, int count, int obj_idx, const char* key,
                               const char* expected) {
    int idx = jstok_object_get(json, tokens, count, obj_idx, key);
    if (idx < 0 || tokens[idx].type != JSTOK_STRING) return false;
    jstok_span_t sp = jstok_span(json, &tokens[idx]);
    size_t expected_len = strlen(expected);
    return sp.n == expected_len && memcmp(sp.p, expected, expected_len) == 0;
}

static bool assert_props_json(const char* json, size_t len, const char* expected_path) {
    jstoktok_t* tokens = NULL;
    int count = 0;
    ASSERT(parse_json_tokens(json, len, &tokens, &count), "parse props JSON");
    ASSERT(tokens[0].type == JSTOK_OBJECT, "props JSON object");
    ASSERT(json_expect_bool(json, tokens, count, 0, "auth", true), "props auth true");
    ASSERT(json_expect_bool(json, tokens, count, 0, "org", true), "props org true");
    ASSERT(json_expect_bool(json, tokens, count, 0, "project", true), "props project true");
    ASSERT(json_expect_bool(json, tokens, count, 0, "custom", true), "props custom true");
    ASSERT(json_expect_string(json, tokens, count, 0, "path", expected_path), "props path");
    free(tokens);
    return true;
}

static bool assert_error_json(const char* json, size_t len, const char* expected_message, const char* expected_type,
                              const char* expected_code) {
    jstoktok_t* tokens = NULL;
    int count = 0;
    ASSERT(parse_json_tokens(json, len, &tokens, &count), "parse error JSON");
    ASSERT(tokens[0].type == JSTOK_OBJECT, "error JSON object");

    int error_idx = jstok_object_get(json, tokens, count, 0, "error");
    ASSERT(error_idx >= 0 && tokens[error_idx].type == JSTOK_OBJECT, "error object");
    ASSERT(json_expect_string(json, tokens, count, error_idx, "message", expected_message), "error message");
    ASSERT(json_expect_string(json, tokens, count, error_idx, "type", expected_type), "error type");
    ASSERT(json_expect_string(json, tokens, count, error_idx, "code", expected_code), "error code");

    free(tokens);
    return true;
}

static bool json_unescape_alloc(const char* escaped, size_t escaped_len, char** out, size_t* out_len) {
    size_t tmp_len = escaped_len + 2;
    char* tmp = malloc(tmp_len + 1);
    if (!tmp) return false;
    tmp[0] = '"';
    memcpy(tmp + 1, escaped, escaped_len);
    tmp[1 + escaped_len] = '"';
    tmp[tmp_len] = '\0';

    jstok_parser parser;
    jstok_init(&parser);
    int needed = jstok_parse(&parser, tmp, (int)tmp_len, NULL, 0);
    if (needed <= 0) {
        free(tmp);
        return false;
    }

    jstoktok_t* tokens = malloc((size_t)needed * sizeof(jstoktok_t));
    if (!tokens) {
        free(tmp);
        return false;
    }

    jstok_init(&parser);
    int parsed = jstok_parse(&parser, tmp, (int)tmp_len, tokens, needed);
    if (parsed <= 0 || tokens[0].type != JSTOK_STRING) {
        free(tokens);
        free(tmp);
        return false;
    }

    size_t cap = escaped_len + 1;
    char* unescaped = malloc(cap + 1);
    if (!unescaped) {
        free(tokens);
        free(tmp);
        return false;
    }

    size_t unescaped_len = 0;
    if (jstok_unescape(tmp, &tokens[0], unescaped, cap, &unescaped_len) != 0) {
        free(unescaped);
        free(tokens);
        free(tmp);
        return false;
    }
    unescaped[unescaped_len] = '\0';

    free(tokens);
    free(tmp);

    *out = unescaped;
    *out_len = unescaped_len;
    return true;
}

static bool assert_auth_payload(const char* escaped, size_t len, const char* expected_mode) {
    char* unescaped = NULL;
    size_t unescaped_len = 0;
    ASSERT(json_unescape_alloc(escaped, len, &unescaped, &unescaped_len), "unescape content");

    jstoktok_t* tokens = NULL;
    int count = 0;
    ASSERT(parse_json_tokens(unescaped, unescaped_len, &tokens, &count), "parse content JSON");
    ASSERT(tokens[0].type == JSTOK_OBJECT, "content JSON object");
    ASSERT(json_expect_bool(unescaped, tokens, count, 0, "auth", true), "content auth true");
    ASSERT(json_expect_bool(unescaped, tokens, count, 0, "org", true), "content org true");
    ASSERT(json_expect_bool(unescaped, tokens, count, 0, "project", true), "content project true");
    ASSERT(json_expect_bool(unescaped, tokens, count, 0, "custom", true), "content custom true");
    ASSERT(json_expect_string(unescaped, tokens, count, 0, "mode", expected_mode), "content mode");

    free(tokens);
    free(unescaped);
    return true;
}

static bool assert_tool_args(const char* escaped, size_t len) {
    char* unescaped = NULL;
    size_t unescaped_len = 0;
    ASSERT(json_unescape_alloc(escaped, len, &unescaped, &unescaped_len), "unescape tool args");

    jstoktok_t* tokens = NULL;
    int count = 0;
    ASSERT(parse_json_tokens(unescaped, unescaped_len, &tokens, &count), "parse tool args JSON");
    ASSERT(tokens[0].type == JSTOK_OBJECT, "tool args JSON object");
    ASSERT(json_expect_bool(unescaped, tokens, count, 0, "auth", true), "tool args auth true");
    ASSERT(json_expect_bool(unescaped, tokens, count, 0, "org", true), "tool args org true");
    ASSERT(json_expect_bool(unescaped, tokens, count, 0, "project", true), "tool args project true");
    ASSERT(json_expect_bool(unescaped, tokens, count, 0, "custom", true), "tool args custom true");
    ASSERT(json_expect_string(unescaped, tokens, count, 0, "echo", "ok"), "tool args echo");

    free(tokens);
    free(unescaped);
    return true;
}

static void stop_server(pid_t pid) {
    if (pid <= 0) return;
    if (kill(pid, SIGTERM) == 0 || errno == ESRCH) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    uint16_t port = 0;
    int listener = create_listener(&port);
    if (listener < 0) {
        fprintf(stderr, "Failed to create listener\n");
        return 1;
    }

    const char* api_key = "sk-test-123";
    char expected_auth[256];
    snprintf(expected_auth, sizeof(expected_auth), "Bearer %s", api_key);
    const char* override_token = "sk-override-456";
    char override_auth[256];
    snprintf(override_auth, sizeof(override_auth), "Bearer %s", override_token);

    const char* org_value = "org-test";
    const char* project_value = "proj-test";
    const char* custom_value = "custom-test";
    const char* custom_override_props = "custom-override";
    const char* custom_override_chat = "custom-override-chat";
    const char* custom_override_stream = "custom-override-stream";
    const char* headers[] = {
        "OpenAI-Organization: org-test",
        "OpenAI-Project: proj-test",
        "X-Custom-Header: custom-test",
    };
    struct expected_headers expected[] = {
        {expected_auth, org_value, project_value, custom_override_props},
        {override_auth, org_value, project_value, custom_value},
        {expected_auth, org_value, project_value, custom_override_chat},
        {expected_auth, org_value, project_value, custom_override_stream},
        {expected_auth, org_value, project_value, custom_value},
        {expected_auth, NULL, NULL, NULL},
        {expected_auth, NULL, NULL, NULL},
    };

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed\n");
        close(listener);
        return 1;
    }

    if (pid == 0) {
        server_loop(listener, expected, 7);
    }
    close(listener);

    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%u", port);

    int exit_code = 1;
    llm_model_t model = {"test-model"};
    llm_client_t* client =
        llm_client_create_with_headers(base_url, &model, NULL, NULL, headers, sizeof(headers) / sizeof(headers[0]));
    if (!client) {
        fprintf(stderr, "Client creation failed\n");
        stop_server(pid);
        return 1;
    }

    if (!llm_client_set_api_key(client, api_key)) {
        fprintf(stderr, "Failed to set API key\n");
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    char props_header[128];
    char chat_header[128];
    char stream_header[128];
    char auth_override_header[128];
    snprintf(props_header, sizeof(props_header), "X-Custom-Header: %s", custom_override_props);
    snprintf(chat_header, sizeof(chat_header), "X-Custom-Header: %s", custom_override_chat);
    snprintf(stream_header, sizeof(stream_header), "X-Custom-Header: %s", custom_override_stream);
    snprintf(auth_override_header, sizeof(auth_override_header), "Authorization: Bearer %s", override_token);
    const char* props_headers[] = {props_header};
    const char* auth_override_headers[] = {auth_override_header};
    const char* chat_headers[] = {chat_header};
    const char* stream_headers[] = {stream_header};

    const char* props_json = NULL;
    size_t props_len = 0;
    if (!llm_props_get_with_headers(client, &props_json, &props_len, props_headers, 1)) {
        fprintf(stderr, "Props request failed\n");
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    if (!assert_props_json(props_json, props_len, "/props")) {
        free((char*)props_json);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    free((char*)props_json);

    const char* props_auth_json = NULL;
    size_t props_auth_len = 0;
    if (!llm_props_get_with_headers(client, &props_auth_json, &props_auth_len, auth_override_headers, 1)) {
        fprintf(stderr, "Props request with auth override failed\n");
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    if (!assert_props_json(props_auth_json, props_auth_len, "/props")) {
        free((char*)props_auth_json);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    free((char*)props_auth_json);

    llm_message_t messages[] = {{LLM_ROLE_USER, "ping", 4, NULL, 0, NULL, 0, NULL, 0, NULL, 0}};
    llm_chat_result_t result;
    if (!llm_chat_with_headers(client, messages, 1, NULL, NULL, NULL, &result, chat_headers, 1)) {
        fprintf(stderr, "Chat request failed\n");
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    if (result.finish_reason != LLM_FINISH_REASON_TOOL_CALLS || result.tool_calls_count != 1) {
        fprintf(stderr, "Chat result missing tool call\n");
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    if (!assert_auth_payload(result.content, result.content_len, "chat")) {
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    if (result.tool_calls[0].name_len != 4 || memcmp(result.tool_calls[0].name, "ping", 4) != 0) {
        fprintf(stderr, "Tool name mismatch\n");
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    if (!assert_tool_args(result.tool_calls[0].arguments, result.tool_calls[0].arguments_len)) {
        llm_chat_result_free(&result);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    llm_chat_result_free(&result);

    struct stream_capture cap = {0};
    llm_stream_callbacks_t cbs = {0};
    cbs.user_data = &cap;
    cbs.on_content_delta = on_stream_content;

    if (!llm_chat_stream_with_headers(client, messages, 1, NULL, NULL, NULL, &cbs, stream_headers, 1) || cap.failed ||
        !cap.data) {
        fprintf(stderr, "Chat stream failed\n");
        free(cap.data);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    if (!assert_auth_payload(cap.data, cap.len, "stream")) {
        free(cap.data);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    free(cap.data);

    const char* props_json_after = NULL;
    size_t props_len_after = 0;
    if (!llm_props_get(client, &props_json_after, &props_len_after)) {
        fprintf(stderr, "Props request after stream failed\n");
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }
    if (!assert_props_json(props_json_after, props_len_after, "/props")) {
        free((char*)props_json_after);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }
    free((char*)props_json_after);

    llm_client_t* noauth_client = llm_client_create(base_url, &model, NULL, NULL);
    if (!noauth_client) {
        fprintf(stderr, "No-auth client creation failed\n");
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    llm_message_t noauth_messages[] = {{LLM_ROLE_USER, "ping", 4, NULL, 0, NULL, 0, NULL, 0, NULL, 0}};
    llm_chat_result_t noauth_result = {0};
    if (llm_chat(noauth_client, noauth_messages, 1, NULL, NULL, NULL, &noauth_result)) {
        fprintf(stderr, "Chat without auth should fail\n");
        llm_chat_result_free(&noauth_result);
        llm_client_destroy(noauth_client);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }

    const char* error_json = NULL;
    size_t error_len = 0;
    llm_error_detail_t detail = {0};
    llm_error_t err = llm_props_get_ex(noauth_client, &error_json, &error_len, &detail);
    if (err == LLM_ERR_NONE) {
        fprintf(stderr, "Props request without auth should fail\n");
        if (error_json) free((char*)error_json);
        llm_error_detail_free(&detail);
        llm_client_destroy(noauth_client);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }
    if (!detail.raw_body ||
        !assert_error_json(detail.raw_body, detail.raw_body_len, "missing api key", "auth_error", "missing_api_key")) {
        llm_error_detail_free(&detail);
        llm_client_destroy(noauth_client);
        llm_client_destroy(client);
        stop_server(pid);
        return 1;
    }
    llm_error_detail_free(&detail);
    llm_client_destroy(noauth_client);

    llm_client_destroy(client);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "waitpid failed\n");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Server process failed\n");
        return 1;
    }

    printf("Auth header test passed!\n");
    exit_code = 0;
    return exit_code;
}
