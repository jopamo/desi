#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "llm/llm.h"
#include "src/transport_curl.h"

#define JSTOK_HEADER
#include <jstok.h>

struct stream_capture {
    char* data;
    size_t len;
    size_t cap;
    bool failed;
};

static void stream_capture_append(struct stream_capture* cap, const char* data, size_t len) {
    if (cap->failed || len == 0) return;
    size_t needed = cap->len + len + 1;
    if (needed > cap->cap) {
        size_t next_cap = cap->cap ? cap->cap * 2 : 256;
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

static void on_stream_chunk(const char* chunk, size_t len, void* user_data) {
    struct stream_capture* cap = user_data;
    stream_capture_append(cap, chunk, len);
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

static int accept_with_timeout(int listener, int timeout_sec) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(listener, &set);
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    int rc = select(listener + 1, &set, NULL, NULL, &tv);
    if (rc <= 0) return -1;
    return accept(listener, NULL, NULL);
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

static bool header_has_token(const char* buf, size_t header_len, const char* key, const char* token) {
    size_t key_len = strlen(key);
    const char* cursor = buf;
    const char* end = buf + header_len;

    while (cursor < end) {
        const char* line_end = strstr(cursor, "\r\n");
        if (!line_end || line_end > end) break;
        if ((size_t)(line_end - cursor) > key_len + 1 && strncmp(cursor, key, key_len) == 0 && cursor[key_len] == ':') {
            const char* val = cursor + key_len + 1;
            while (val < line_end && (*val == ' ' || *val == '\t')) val++;
            if (strstr(val, token) != NULL) return true;
        }
        cursor = line_end + 2;
    }

    return false;
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

static bool send_json(int fd, const char* source) {
    char body[128];
    int body_len = snprintf(body, sizeof(body), "{\"source\":\"%s\"}", source);
    if (body_len < 0 || (size_t)body_len >= sizeof(body)) return false;

    char header[256];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return false;

    if (!send_all(fd, header, (size_t)header_len)) return false;
    return send_all(fd, body, (size_t)body_len);
}

static bool handle_request(int fd, const char* source) {
    char buf[8192];
    size_t header_len = 0;
    size_t body_len = 0;
    if (!read_request(fd, buf, sizeof(buf), &header_len, &body_len)) return false;
    (void)header_len;
    (void)body_len;
    return send_json(fd, source);
}

static void server_loop(int listener, const char* source, int requests) {
    for (int i = 0; i < requests; i++) {
        int fd = accept_with_timeout(listener, 5);
        if (fd < 0) _exit(1);
        bool ok = handle_request(fd, source);
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

static bool json_expect_source(const char* json, size_t len, const char* expected) {
    jstoktok_t* tokens = NULL;
    int count = 0;
    if (!parse_json_tokens(json, len, &tokens, &count)) return false;
    if (tokens[0].type != JSTOK_OBJECT) {
        free(tokens);
        return false;
    }

    int idx = jstok_object_get(json, tokens, count, 0, "source");
    if (idx < 0 || tokens[idx].type != JSTOK_STRING) {
        free(tokens);
        return false;
    }

    jstok_span_t sp = jstok_span(json, &tokens[idx]);
    size_t expected_len = strlen(expected);
    bool ok = sp.n == expected_len && memcmp(sp.p, expected, expected_len) == 0;
    free(tokens);
    return ok;
}

static bool wait_for_exit(pid_t pid) {
    if (pid <= 0) return true;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void terminate_server(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int exit_code = 1;
    int target_listener = -1;
    int proxy_listener = -1;
    pid_t target_pid = -1;
    pid_t proxy_pid = -1;
    llm_client_t* client = NULL;
    bool ok = true;

    uint16_t target_port = 0;
    uint16_t proxy_port = 0;
    target_listener = create_listener(&target_port);
    if (target_listener < 0) {
        fprintf(stderr, "Failed to create target listener\n");
        return 1;
    }
    proxy_listener = create_listener(&proxy_port);
    if (proxy_listener < 0) {
        fprintf(stderr, "Failed to create proxy listener\n");
        close(target_listener);
        return 1;
    }

    target_pid = fork();
    if (target_pid == 0) {
        close(proxy_listener);
        server_loop(target_listener, "target", 2);
    } else if (target_pid < 0) {
        fprintf(stderr, "Failed to fork target server\n");
        close(target_listener);
        close(proxy_listener);
        return 1;
    }

    proxy_pid = fork();
    if (proxy_pid == 0) {
        close(target_listener);
        server_loop(proxy_listener, "proxy", 3);
    } else if (proxy_pid < 0) {
        fprintf(stderr, "Failed to fork proxy server\n");
        terminate_server(target_pid);
        close(target_listener);
        close(proxy_listener);
        wait_for_exit(target_pid);
        return 1;
    }

    close(target_listener);
    close(proxy_listener);

    char base_url[128];
    char proxy_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%u", (unsigned)target_port);
    snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%u", (unsigned)proxy_port);

    setenv("http_proxy", proxy_url, 1);
    setenv("HTTP_PROXY", proxy_url, 1);
    setenv("no_proxy", "127.0.0.1", 1);
    setenv("NO_PROXY", "127.0.0.1", 1);

    llm_model_t model = {"test-model"};
    client = llm_client_create(base_url, &model, NULL, NULL);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        ok = false;
        goto cleanup;
    }

    const char* props = NULL;
    size_t props_len = 0;
    if (!llm_props_get(client, &props, &props_len)) {
        fprintf(stderr, "Props request without proxy failed\n");
        ok = false;
        goto cleanup;
    }
    if (!json_expect_source(props, props_len, "target")) {
        fprintf(stderr, "Expected direct request to hit target\n");
        free((void*)props);
        ok = false;
        goto cleanup;
    }
    free((void*)props);
    props = NULL;

    if (!llm_client_set_proxy(client, proxy_url)) {
        fprintf(stderr, "Failed to set proxy URL\n");
        ok = false;
        goto cleanup;
    }

    if (!llm_props_get(client, &props, &props_len)) {
        fprintf(stderr, "Props request with proxy failed\n");
        ok = false;
        goto cleanup;
    }
    if (!json_expect_source(props, props_len, "proxy")) {
        fprintf(stderr, "Expected proxied request to hit proxy\n");
        free((void*)props);
        ok = false;
        goto cleanup;
    }
    free((void*)props);
    props = NULL;

    if (!llm_client_set_no_proxy(client, "127.0.0.1")) {
        fprintf(stderr, "Failed to set no-proxy list\n");
        ok = false;
        goto cleanup;
    }

    if (!llm_props_get(client, &props, &props_len)) {
        fprintf(stderr, "Props request with no-proxy failed\n");
        ok = false;
        goto cleanup;
    }
    if (!json_expect_source(props, props_len, "target")) {
        fprintf(stderr, "Expected no-proxy request to hit target\n");
        free((void*)props);
        ok = false;
        goto cleanup;
    }
    free((void*)props);
    props = NULL;

    if (!llm_client_set_no_proxy(client, NULL)) {
        fprintf(stderr, "Failed to clear no-proxy list\n");
        ok = false;
        goto cleanup;
    }

    char post_url[256];
    snprintf(post_url, sizeof(post_url), "%s/post", base_url);
    char* body = NULL;
    size_t body_len = 0;
    if (!http_post(post_url, "{}", 1000, 1024, NULL, 0, NULL, proxy_url, NULL, &body, &body_len)) {
        fprintf(stderr, "http_post via proxy failed\n");
        ok = false;
        goto cleanup;
    }
    if (!json_expect_source(body, body_len, "proxy")) {
        fprintf(stderr, "Expected http_post to hit proxy\n");
        free(body);
        ok = false;
        goto cleanup;
    }
    free(body);
    body = NULL;

    char stream_url[256];
    snprintf(stream_url, sizeof(stream_url), "%s/stream", base_url);
    struct stream_capture cap = {0};
    if (!http_post_stream(stream_url, "{}", 1000, 1000, NULL, 0, NULL, proxy_url, NULL, on_stream_chunk, &cap) ||
        cap.failed || !cap.data) {
        fprintf(stderr, "http_post_stream via proxy failed\n");
        ok = false;
        goto cleanup;
    }
    if (!json_expect_source(cap.data, cap.len, "proxy")) {
        fprintf(stderr, "Expected http_post_stream to hit proxy\n");
        free(cap.data);
        ok = false;
        goto cleanup;
    }
    free(cap.data);
    cap.data = NULL;

cleanup:
    if (client) llm_client_destroy(client);
    if (!ok) {
        terminate_server(target_pid);
        terminate_server(proxy_pid);
    }

    bool target_ok = wait_for_exit(target_pid);
    bool proxy_ok = wait_for_exit(proxy_pid);
    if (ok && target_ok && proxy_ok) {
        exit_code = 0;
    } else {
        exit_code = 1;
    }

    return exit_code;
}
