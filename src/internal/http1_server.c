#include "http1_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { DESI_HTTP_MAX_HEADER_BYTES = 8192 };

static const char* desi_reason_phrase(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        case 500:
            return "Internal Server Error";
        default:
            return "OK";
    }
}

static int desi_write_all(int fd, const char* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t wrote = send(fd, buf + off, len - off, 0);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (wrote == 0) return -1;
        off += (size_t)wrote;
    }
    return 0;
}

static int desi_send_response(int fd, const desi_http_resp_t* resp) {
    const char* body = resp->body;
    size_t body_len = resp->body_len;
    int status = resp->status;
    const char* content_type = resp->content_type;
    static const char k_default_type[] = "text/plain; charset=utf-8";
    static const char k_internal_body[] = "Internal Server Error\n";

    if (status == 0) status = 200;
    if (body_len > 0 && body == NULL) {
        status = 500;
        body = k_internal_body;
        body_len = sizeof(k_internal_body) - 1;
        content_type = k_default_type;
    }
    if (body_len > 0 && content_type == NULL) {
        content_type = k_default_type;
    }

    char header[512];
    int header_len = 0;
    if (body_len > 0 && content_type) {
        header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              status, desi_reason_phrase(status), content_type, body_len);
    } else {
        header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              status, desi_reason_phrase(status), body_len);
    }
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return -1;
    if (desi_write_all(fd, header, (size_t)header_len) != 0) return -1;
    if (body_len > 0) {
        return desi_write_all(fd, body, body_len);
    }
    return 0;
}

static bool desi_next_line(const char* buf, size_t len, size_t start, size_t* line_end, size_t* next) {
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

static bool desi_find_header_end(const char* buf, size_t len, size_t* header_end) {
    size_t pos = 0;
    size_t line_end = 0;
    size_t next = 0;
    while (desi_next_line(buf, len, pos, &line_end, &next)) {
        if (line_end == pos) {
            *header_end = next;
            return true;
        }
        pos = next;
    }
    return false;
}

static int desi_parse_request_line(const char* buf, size_t len, desi_http_req_t* req) {
    size_t line_end = 0;
    size_t next = 0;
    if (!desi_next_line(buf, len, 0, &line_end, &next)) return -1;
    size_t method_end = 0;
    while (method_end < line_end && buf[method_end] != ' ') {
        method_end++;
    }
    if (method_end == 0 || method_end >= line_end) return -1;
    size_t path_start = method_end + 1;
    if (path_start >= line_end) return -1;
    size_t path_end = path_start;
    while (path_end < line_end && buf[path_end] != ' ') {
        path_end++;
    }
    if (path_end == path_start || path_end >= line_end) return -1;
    req->method = buf;
    req->method_len = method_end;
    req->path = buf + path_start;
    req->path_len = path_end - path_start;
    return 0;
}

static int desi_read_request(int fd, char* buf, size_t cap, size_t* out_header_end) {
    size_t len = 0;
    while (len < cap) {
        size_t header_end = 0;
        if (desi_find_header_end(buf, len, &header_end)) {
            *out_header_end = header_end;
            return 0;
        }
        ssize_t got = recv(fd, buf + len, cap - len, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) return -1;
        len += (size_t)got;
    }
    size_t header_end = 0;
    if (desi_find_header_end(buf, len, &header_end)) {
        *out_header_end = header_end;
        return 0;
    }
    return 1;
}

static int desi_handle_client(int fd, desi_request_handler_t handler, void* user_data) {
    char buf[DESI_HTTP_MAX_HEADER_BYTES];
    size_t header_end = 0;
    int rc = desi_read_request(fd, buf, sizeof(buf), &header_end);
    if (rc == 1) {
        desi_http_resp_t resp = {.status = 413, .body = "Request Too Large\n", .body_len = 18};
        return desi_send_response(fd, &resp);
    }
    if (rc != 0) return -1;

    desi_http_req_t req = {0};
    if (desi_parse_request_line(buf, header_end, &req) != 0) {
        desi_http_resp_t resp = {.status = 400, .body = "Bad Request\n", .body_len = 12};
        return desi_send_response(fd, &resp);
    }

    desi_http_resp_t resp = {0};
    int handler_rc = handler(user_data, &req, &resp);
    if (handler_rc < 0) {
        resp.status = 500;
        resp.body = "Internal Server Error\n";
        resp.body_len = 22;
    }
    return desi_send_response(fd, &resp);
}

static int desi_listen_socket(const desi_server_config_t* conf) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(conf->port);
    if (!conf->bind_host || conf->bind_host[0] == '\0' || strcmp(conf->bind_host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (strcmp(conf->bind_host, "127.0.0.1") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        if (inet_pton(AF_INET, conf->bind_host, &addr.sin_addr) != 1) {
            close(fd);
            return -1;
        }
    }

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    int backlog = conf->backlog > 0 ? conf->backlog : 128;
    if (listen(fd, backlog) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int desi_server_run(const desi_server_config_t* conf, desi_request_handler_t handler, void* user_data) {
    if (!conf || !handler) return -1;

    signal(SIGPIPE, SIG_IGN);

    int listen_fd = desi_listen_socket(conf);
    if (listen_fd < 0) return -1;

    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            close(listen_fd);
            return -1;
        }

        (void)desi_handle_client(client_fd, handler, user_data);
        close(client_fd);
    }

    return 0;
}
