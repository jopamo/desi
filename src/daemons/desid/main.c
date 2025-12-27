#include <stdio.h>
#include <string.h>

#include "http1_server.h"

static int path_is(const desi_http_req_t* req, const char* target) {
    size_t len = strlen(target);
    return (req->path_len == len && memcmp(req->path, target, len) == 0);
}

static int desid_handler(void* ctx, const desi_http_req_t* req, desi_http_resp_t* resp) {
    (void)ctx;

    if (path_is(req, "/health")) {
        if (req->method_len == 3 && memcmp(req->method, "GET", 3) == 0) {
            resp->status = 200;
            resp->body = "ok\n";
            resp->body_len = 3;
            return 0;
        }
        resp->status = 405;
        return 0;
    }

    resp->status = 404;
    resp->body = "Not Found\n";
    resp->body_len = 10;
    return 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    desi_server_config_t conf = {.bind_host = "127.0.0.1", .port = 8081, .backlog = 128, .idle_timeout_ms = 5000};

    fprintf(stderr, "[desid] Starting on %s:%d\n", conf.bind_host, conf.port);
    return desi_server_run(&conf, desid_handler, NULL);
}
