#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/transport_curl.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int main(void) {
    const char* test_filename = "transport_test_temp.txt";
    FILE* f = fopen(test_filename, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fprintf(f, "hello world\n");
    fclose(f);

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd() error");
        return 1;
    }

    char url[PATH_MAX + 128];
    snprintf(url, sizeof(url), "file://%s/%s", cwd, test_filename);

    char* body = NULL;
    size_t len = 0;
    llm_transport_status_t status;
    if (!http_get(url, 1000, 1024, NULL, 0, NULL, NULL, NULL, &body, &len, &status)) {
        fprintf(stderr, "http_get failed\n");
        return 1;
    }

    printf("Received: %s", body);
    printf("Length: %zu\n", len);

    if (strcmp(body, "hello world\n") != 0) {
        fprintf(stderr, "Content mismatch\n");
        free(body);
        return 1;
    }

    free(body);
    body = NULL;
    len = 0;
    if (http_get(url, 1000, 5, NULL, 0, NULL, NULL, NULL, &body, &len, &status)) {
        fprintf(stderr, "http_get should have failed due to max_response_bytes\n");
        free(body);
        remove(test_filename);
        return 1;
    }

    printf("Cap test passed!\n");
    remove(test_filename);
    printf("Transport test passed!\n");
    return 0;
}
