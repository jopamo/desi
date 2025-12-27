#ifndef DESI_INTERNAL_HTTP1_SERVER_H
#define DESI_INTERNAL_HTTP1_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* bind_host;
    uint16_t port;
    int backlog;
    uint32_t idle_timeout_ms;
} desi_server_config_t;

typedef struct {
    const char* method;
    size_t method_len;
    const char* path;
    size_t path_len;
} desi_http_req_t;

typedef struct {
    int status;
    const char* content_type;
    const char* body;
    size_t body_len;
} desi_http_resp_t;

typedef int (*desi_request_handler_t)(void* user_data, const desi_http_req_t* req, desi_http_resp_t* resp);

int desi_server_run(const desi_server_config_t* conf, desi_request_handler_t handler, void* user_data);

#endif
