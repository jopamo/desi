#ifndef TRANSPORT_CURL_H
#define TRANSPORT_CURL_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*stream_cb)(const char* chunk, size_t len, void* user_data);

bool http_get(const char* url, long timeout_ms, size_t max_response_bytes, char** body, size_t* len);

bool http_post(const char* url, const char* json_body, long timeout_ms, size_t max_response_bytes, char** body,
               size_t* len);

bool http_post_stream(const char* url, const char* json_body, long timeout_ms, long read_idle_timeout_ms, stream_cb cb,
                      void* user_data);

#endif  // TRANSPORT_CURL_H
