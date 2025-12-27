#include <stdio.h>
#include <string.h>

#include "http1_server.h"

static int mcpd_handler(void* ctx, const desi_http_req_t* req, desi_http_resp_t* resp) {
    (void)ctx;

    int is_get = (req->method_len == 3 && memcmp(req->method, "GET", 3) == 0);

    size_t len = strlen("/health");
    if (req->path_len == len && memcmp(req->path, "/health", len) == 0) {
        if (!is_get) {
            resp->status = 405;
            return 0;
        }
        resp->status = 200;
        resp->body = "mcp_active\n";
        resp->body_len = 11;
        return 0;
    }

    resp->status = 404;
    return 0;
}

int main(void) {
    desi_server_config_t conf = {.bind_host = "127.0.0.1", .port = 8082, .backlog = 128, .idle_timeout_ms = 5000};

    fprintf(stderr, "[mcpd] Starting on %s:%d\n", conf.bind_host, conf.port);
    return desi_server_run(&conf, mcpd_handler, NULL);
}
