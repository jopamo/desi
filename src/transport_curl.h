#ifndef TRANSPORT_CURL_H
#define TRANSPORT_CURL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct llm_tls_config llm_tls_config_t;

typedef bool (*stream_cb)(const char* chunk, size_t len, void* user_data);

typedef struct {
    long http_status;
    int curl_code;
    bool tls_error;
} llm_transport_status_t;

bool http_get(const char* url, long timeout_ms, size_t max_response_bytes, const char* const* headers,
              size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url, const char* no_proxy,
              char** body, size_t* len, llm_transport_status_t* status);

bool http_post(const char* url, const char* json_body, long timeout_ms, size_t max_response_bytes,
               const char* const* headers, size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url,
               const char* no_proxy, char** body, size_t* len, llm_transport_status_t* status);

bool http_post_stream(const char* url, const char* json_body, long timeout_ms, long read_idle_timeout_ms,
                      const char* const* headers, size_t headers_count, const llm_tls_config_t* tls,
                      const char* proxy_url, const char* no_proxy, stream_cb cb, void* user_data,
                      llm_transport_status_t* status);

#endif  // TRANSPORT_CURL_H
