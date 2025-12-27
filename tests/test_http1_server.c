#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/internal/http1_server.c"

struct handler_state {
    int called;
};

static int health_handler(void* user_data, const desi_http_req_t* req, desi_http_resp_t* resp) {
    struct handler_state* state = user_data;
    if (state) state->called++;

    if (req->method_len != 3 || memcmp(req->method, "GET", 3) != 0) return -1;
    if (req->path_len != 7 || memcmp(req->path, "/health", 7) != 0) return -1;

    resp->status = 200;
    resp->body = "ok\n";
    resp->body_len = 3;
    return 0;
}

static bool next_line(const char* buf, size_t len, size_t start, size_t* line_end, size_t* next) {
    size_t i = start;
    while (i < len && buf[i] != '\n' && buf[i] != '\r') {
        i++;
    }
    if (i == len) return false;
    *line_end = i;
    i++;
    if (buf[*line_end] == '\r' && i < len && buf[i] == '\n') {
        i++;
    }
    *next = i;
    return true;
}

static int parse_status_line(const char* line, size_t len, int* out_status) {
    size_t i = 0;
    while (i < len && line[i] != ' ') {
        i++;
    }
    if (i == len) return -1;
    i++;
    int status = 0;
    int digits = 0;
    while (i < len && line[i] >= '0' && line[i] <= '9') {
        status = status * 10 + (line[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0) return -1;
    *out_status = status;
    return 0;
}

static int parse_content_length(const char* line, size_t len, size_t* out_len) {
    const char key[] = "Content-Length:";
    size_t key_len = sizeof(key) - 1;
    if (len < key_len) return -1;
    if (memcmp(line, key, key_len) != 0) return -1;
    size_t i = key_len;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i == len) return -1;
    size_t value = 0;
    int digits = 0;
    while (i < len && line[i] >= '0' && line[i] <= '9') {
        value = value * 10 + (size_t)(line[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0) return -1;
    *out_len = value;
    return 0;
}

struct http_response {
    int status;
    size_t content_length;
    const char* body;
    size_t body_len;
};

static int parse_response(const char* buf, size_t len, struct http_response* out) {
    size_t pos = 0;
    size_t line_end = 0;
    size_t next = 0;
    if (!next_line(buf, len, pos, &line_end, &next)) return -1;
    if (parse_status_line(buf + pos, line_end - pos, &out->status) != 0) return -1;
    pos = next;
    out->content_length = 0;
    while (next_line(buf, len, pos, &line_end, &next)) {
        if (line_end == pos) {
            pos = next;
            out->body = buf + pos;
            out->body_len = len - pos;
            return 0;
        }
        size_t value = 0;
        if (parse_content_length(buf + pos, line_end - pos, &value) == 0) {
            out->content_length = value;
        }
        pos = next;
    }
    return -1;
}

static int run_health_test(void) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        perror("socketpair");
        return 1;
    }

    const char req[] = "GET /health HTTP/1.1\r\nHost: example\r\n\r\n";
    if (write(fds[0], req, sizeof(req) - 1) < 0) {
        perror("write");
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    struct handler_state state = {0};
    if (desi_handle_client(fds[1], health_handler, &state) != 0) {
        fprintf(stderr, "health handler failed\n");
        close(fds[0]);
        close(fds[1]);
        return 1;
    }
    close(fds[1]);

    char buf[256];
    ssize_t got = read(fds[0], buf, sizeof(buf) - 1);
    if (got <= 0) {
        perror("read");
        close(fds[0]);
        return 1;
    }
    buf[got] = '\0';
    close(fds[0]);

    if (state.called != 1) {
        fprintf(stderr, "handler called %d times\n", state.called);
        return 1;
    }

    struct http_response resp = {0};
    if (parse_response(buf, (size_t)got, &resp) != 0) {
        fprintf(stderr, "response parse failed\n");
        return 1;
    }
    if (resp.status != 200) {
        fprintf(stderr, "unexpected status %d\n", resp.status);
        return 1;
    }
    if (resp.content_length != 3) {
        fprintf(stderr, "unexpected content length %zu\n", resp.content_length);
        return 1;
    }
    if (resp.body_len < resp.content_length) {
        fprintf(stderr, "short body\n");
        return 1;
    }
    if (memcmp(resp.body, "ok\n", 3) != 0) {
        fprintf(stderr, "unexpected body\n");
        return 1;
    }

    return 0;
}

static int run_bad_request_test(void) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        perror("socketpair");
        return 1;
    }

    const char req[] = "GET /health\r\n\r\n";
    if (write(fds[0], req, sizeof(req) - 1) < 0) {
        perror("write");
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    struct handler_state state = {0};
    if (desi_handle_client(fds[1], health_handler, &state) != 0) {
        fprintf(stderr, "bad request handling failed\n");
        close(fds[0]);
        close(fds[1]);
        return 1;
    }
    close(fds[1]);

    char buf[256];
    ssize_t got = read(fds[0], buf, sizeof(buf) - 1);
    if (got <= 0) {
        perror("read");
        close(fds[0]);
        return 1;
    }
    buf[got] = '\0';
    close(fds[0]);

    if (state.called != 0) {
        fprintf(stderr, "handler should not be called for bad request\n");
        return 1;
    }

    struct http_response resp = {0};
    if (parse_response(buf, (size_t)got, &resp) != 0) {
        fprintf(stderr, "response parse failed\n");
        return 1;
    }
    if (resp.status != 400) {
        fprintf(stderr, "unexpected status %d\n", resp.status);
        return 1;
    }
    if (resp.content_length != 12) {
        fprintf(stderr, "unexpected content length %zu\n", resp.content_length);
        return 1;
    }
    if (resp.body_len < resp.content_length) {
        fprintf(stderr, "short body\n");
        return 1;
    }
    if (memcmp(resp.body, "Bad Request\n", resp.content_length) != 0) {
        fprintf(stderr, "unexpected body\n");
        return 1;
    }

    return 0;
}

int main(void) {
    if (run_health_test() != 0) return 1;
    if (run_bad_request_test() != 0) return 1;
    printf("http1 server tests passed\n");
    return 0;
}
