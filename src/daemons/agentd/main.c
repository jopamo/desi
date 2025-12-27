#include <stdio.h>
#include <string.h>

#include "http1_server.h"

static int path_is(const desi_http_req_t* req, const char* target) {
    size_t len = strlen(target);
    return (req->path_len == len && memcmp(req->path, target, len) == 0);
}

static int agentd_handler(void* ctx, const desi_http_req_t* req, desi_http_resp_t* resp) {
    (void)ctx;

    if (path_is(req, "/health")) {
        resp->status = 200;
        resp->body = "agent_ok\n";
        resp->body_len = 9;
        return 0;
    }

    resp->status = 404;
    return 0;
}

int main(void) {
    desi_server_config_t conf = {.bind_host = "0.0.0.0", .port = 8080, .backlog = 1024, .idle_timeout_ms = 5000};

    fprintf(stderr, "[agentd] Starting on %s:%d\n", conf.bind_host, conf.port);
    return desi_server_run(&conf, agentd_handler, NULL);
}
