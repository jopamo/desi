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
#include <time.h>
#include <unistd.h>

#include "llm/llm.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static bool run_openssl(char* const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool generate_cert(const char* cert_path, const char* key_path) {
    char* const argv[] = {
        "openssl",  "req",
        "-x509",    "-newkey",
        "rsa:2048", "-nodes",
        "-keyout",  (char*)key_path,
        "-out",     (char*)cert_path,
        "-addext",  "subjectAltName=DNS:localhost",
        "-addext",  "basicConstraints=CA:TRUE",
        "-addext",  "keyUsage=keyCertSign,digitalSignature,keyEncipherment",
        "-addext",  "extendedKeyUsage=serverAuth",
        "-subj",    "/CN=localhost",
        "-days",    "1",
        NULL,
    };

    return run_openssl(argv);
}

static bool reserve_port(uint16_t* port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) != 0) {
        close(fd);
        return false;
    }

    *port_out = ntohs(addr.sin_port);
    close(fd);
    return true;
}

static pid_t start_tls_server(const char* cert_path, const char* key_path, uint16_t port) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
        execlp("openssl", "openssl", "s_server", "-accept", port_str, "-key", key_path, "-cert", cert_path, "-www",
               "-quiet", NULL);
        _exit(127);
    }
    return pid;
}

static bool wait_for_port(uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    for (int i = 0; i < 50; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                close(fd);
                return true;
            }
            close(fd);
        }
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100000000L;
        nanosleep(&ts, NULL);
    }

    return false;
}

static void stop_tls_server(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

int main(void) {
    // Avoid external CA environment overrides in this test.
    unsetenv("SSL_CERT_FILE");
    unsetenv("SSL_CERT_DIR");
    unsetenv("CURL_CA_BUNDLE");

    int status = 1;
    char tempdir[] = "/tmp/desi_tls_test_XXXXXX";
    char cert_path[PATH_MAX];
    char key_path[PATH_MAX];
    char base_url[256];
    pid_t server_pid = -1;
    llm_client_t* client = NULL;

    if (!mkdtemp(tempdir)) {
        perror("mkdtemp");
        return 1;
    }

    snprintf(cert_path, sizeof(cert_path), "%s/cert.pem", tempdir);
    snprintf(key_path, sizeof(key_path), "%s/key.pem", tempdir);

    if (!generate_cert(cert_path, key_path)) {
        fprintf(stderr, "Failed to generate TLS cert\n");
        goto cleanup;
    }

    uint16_t port = 0;
    if (!reserve_port(&port)) {
        fprintf(stderr, "Failed to reserve port\n");
        goto cleanup;
    }

    server_pid = start_tls_server(cert_path, key_path, port);
    if (server_pid < 0) {
        fprintf(stderr, "Failed to start TLS server\n");
        goto cleanup;
    }

    if (!wait_for_port(port)) {
        fprintf(stderr, "TLS server did not start\n");
        goto cleanup;
    }

    snprintf(base_url, sizeof(base_url), "https://localhost:%u", (unsigned)port);

    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {0};
    timeout.connect_timeout_ms = 1000;
    timeout.overall_timeout_ms = 2000;

    llm_limits_t limits = {0};
    limits.max_response_bytes = 64 * 1024;
    limits.max_line_bytes = 1024;
    limits.max_tool_args_bytes_per_call = 1024;

    client = llm_client_create(base_url, &model, &timeout, &limits);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        goto cleanup;
    }

    const char* body = NULL;
    size_t body_len = 0;
    if (llm_props_get(client, &body, &body_len)) {
        fprintf(stderr, "Unexpected TLS success without config\n");
        free((void*)body);
        goto cleanup;
    }

    llm_tls_config_t tls = {0};
    tls.ca_bundle_path = cert_path;
    if (!llm_client_set_tls_config(client, &tls)) {
        fprintf(stderr, "Failed to set CA bundle\n");
        goto cleanup;
    }

    if (!llm_props_get(client, &body, &body_len)) {
        fprintf(stderr, "TLS failed with CA bundle\n");
        goto cleanup;
    }
    free((void*)body);
    body = NULL;

    llm_tls_config_t insecure = {0};
    insecure.insecure = true;
    if (!llm_client_set_tls_config(client, &insecure)) {
        fprintf(stderr, "Failed to set insecure TLS\n");
        goto cleanup;
    }

    if (!llm_props_get(client, &body, &body_len)) {
        fprintf(stderr, "TLS failed in insecure mode\n");
        goto cleanup;
    }
    free((void*)body);
    body = NULL;

    status = 0;

cleanup:
    if (client) llm_client_destroy(client);
    stop_tls_server(server_pid);
    remove(cert_path);
    remove(key_path);
    rmdir(tempdir);
    return status;
}
