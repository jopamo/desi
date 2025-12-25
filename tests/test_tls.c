#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

static bool copy_file(const char* src_path, const char* dst_path) {
    int in_fd = open(src_path, O_RDONLY);
    if (in_fd < 0) return false;
    int out_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        close(in_fd);
        return false;
    }

    char buf[4096];
    while (true) {
        ssize_t read_len = read(in_fd, buf, sizeof(buf));
        if (read_len < 0) {
            close(in_fd);
            close(out_fd);
            return false;
        }
        if (read_len == 0) break;
        size_t offset = 0;
        while (offset < (size_t)read_len) {
            ssize_t write_len = write(out_fd, buf + offset, (size_t)read_len - offset);
            if (write_len <= 0) {
                close(in_fd);
                close(out_fd);
                return false;
            }
            offset += (size_t)write_len;
        }
    }

    close(in_fd);
    close(out_fd);
    return true;
}

static bool rehash_ca_dir(const char* ca_dir) {
    char* const argv[] = {"openssl", "rehash", (char*)ca_dir, NULL};
    return run_openssl(argv);
}

static void cleanup_dir(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) return;
    struct dirent* entry = NULL;
    char buf[PATH_MAX];
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        int len = snprintf(buf, sizeof(buf), "%s/%s", path, entry->d_name);
        if (len < 0 || (size_t)len >= sizeof(buf)) continue;
        unlink(buf);
    }
    closedir(dir);
    rmdir(path);
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
    int status = 1;
    char tempdir[] = "/tmp/desi_tls_test_XXXXXX";
    char cert_path[PATH_MAX];
    char key_path[PATH_MAX];
    char ca_dir[PATH_MAX];
    char ca_cert_path[PATH_MAX];
    char base_url_localhost[256];
    char base_url_ip[256];
    pid_t server_pid = -1;
    llm_client_t* client_localhost = NULL;
    llm_client_t* client_ip = NULL;

    ca_dir[0] = '\0';

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

    snprintf(ca_dir, sizeof(ca_dir), "%s/ca", tempdir);
    if (mkdir(ca_dir, 0700) != 0) {
        fprintf(stderr, "Failed to create CA directory\n");
        goto cleanup;
    }

    snprintf(ca_cert_path, sizeof(ca_cert_path), "%s/ca.pem", ca_dir);
    if (!copy_file(cert_path, ca_cert_path)) {
        fprintf(stderr, "Failed to stage CA bundle\n");
        goto cleanup;
    }

    if (!rehash_ca_dir(ca_dir)) {
        fprintf(stderr, "Failed to rehash CA directory\n");
        goto cleanup;
    }

    if (setenv("CURL_CA_BUNDLE", cert_path, 1) != 0 || setenv("SSL_CERT_FILE", cert_path, 1) != 0) {
        fprintf(stderr, "Failed to set CA environment\n");
        goto cleanup;
    }
    unsetenv("SSL_CERT_DIR");

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

    snprintf(base_url_localhost, sizeof(base_url_localhost), "https://localhost:%u", (unsigned)port);
    snprintf(base_url_ip, sizeof(base_url_ip), "https://127.0.0.1:%u", (unsigned)port);

    llm_model_t model = {"test-model"};
    llm_timeout_t timeout = {0};
    timeout.connect_timeout_ms = 1000;
    timeout.overall_timeout_ms = 2000;

    llm_limits_t limits = {0};
    limits.max_response_bytes = 64 * 1024;
    limits.max_line_bytes = 1024;
    limits.max_tool_args_bytes_per_call = 1024;

    client_localhost = llm_client_create(base_url_localhost, &model, &timeout, &limits);
    if (!client_localhost) {
        fprintf(stderr, "Failed to create localhost client\n");
        goto cleanup;
    }

    client_ip = llm_client_create(base_url_ip, &model, &timeout, &limits);
    if (!client_ip) {
        fprintf(stderr, "Failed to create IP client\n");
        goto cleanup;
    }

    const char* body = NULL;
    size_t body_len = 0;
    if (llm_props_get(client_localhost, &body, &body_len)) {
        fprintf(stderr, "Unexpected TLS success without explicit config\n");
        free((void*)body);
        goto cleanup;
    }

    llm_tls_config_t tls_bundle = {0};
    tls_bundle.ca_bundle_path = cert_path;
    if (!llm_client_set_tls_config(client_localhost, &tls_bundle)) {
        fprintf(stderr, "Failed to set CA bundle\n");
        goto cleanup;
    }

    if (!llm_props_get(client_localhost, &body, &body_len)) {
        fprintf(stderr, "TLS failed with CA bundle\n");
        goto cleanup;
    }
    free((void*)body);
    body = NULL;

    llm_tls_config_t tls_dir = {0};
    tls_dir.ca_dir_path = ca_dir;
    if (!llm_client_set_tls_config(client_localhost, &tls_dir)) {
        fprintf(stderr, "Failed to set CA directory\n");
        goto cleanup;
    }

    if (!llm_props_get(client_localhost, &body, &body_len)) {
        fprintf(stderr, "TLS failed with CA directory\n");
        goto cleanup;
    }
    free((void*)body);
    body = NULL;

    llm_tls_config_t tls_no_peer = {0};
    tls_no_peer.verify_peer = LLM_TLS_VERIFY_OFF;
    tls_no_peer.verify_host = LLM_TLS_VERIFY_ON;
    if (!llm_client_set_tls_config(client_localhost, &tls_no_peer)) {
        fprintf(stderr, "Failed to disable peer verification\n");
        goto cleanup;
    }

    if (!llm_props_get(client_localhost, &body, &body_len)) {
        fprintf(stderr, "TLS failed with peer verification disabled\n");
        goto cleanup;
    }
    free((void*)body);
    body = NULL;

    llm_tls_config_t tls_host_on = {0};
    tls_host_on.ca_bundle_path = cert_path;
    tls_host_on.verify_peer = LLM_TLS_VERIFY_ON;
    tls_host_on.verify_host = LLM_TLS_VERIFY_ON;
    if (!llm_client_set_tls_config(client_ip, &tls_host_on)) {
        fprintf(stderr, "Failed to set host verification on\n");
        goto cleanup;
    }

    if (llm_props_get(client_ip, &body, &body_len)) {
        fprintf(stderr, "Unexpected TLS success with host mismatch\n");
        free((void*)body);
        goto cleanup;
    }

    llm_tls_config_t tls_host_off = tls_host_on;
    tls_host_off.verify_host = LLM_TLS_VERIFY_OFF;
    if (!llm_client_set_tls_config(client_ip, &tls_host_off)) {
        fprintf(stderr, "Failed to disable host verification\n");
        goto cleanup;
    }

    if (!llm_props_get(client_ip, &body, &body_len)) {
        fprintf(stderr, "TLS failed with host verification disabled\n");
        goto cleanup;
    }
    free((void*)body);
    body = NULL;

    llm_tls_config_t insecure = {0};
    insecure.insecure = true;
    if (!llm_client_set_tls_config(client_ip, &insecure)) {
        fprintf(stderr, "Failed to set insecure TLS\n");
        goto cleanup;
    }

    if (!llm_props_get(client_ip, &body, &body_len)) {
        fprintf(stderr, "TLS failed in insecure mode\n");
        goto cleanup;
    }
    free((void*)body);
    body = NULL;

    status = 0;

cleanup:
    if (client_localhost) llm_client_destroy(client_localhost);
    if (client_ip) llm_client_destroy(client_ip);
    stop_tls_server(server_pid);
    if (ca_dir[0]) cleanup_dir(ca_dir);
    remove(cert_path);
    remove(key_path);
    rmdir(tempdir);
    return status;
}
