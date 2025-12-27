#ifndef FAKE_TRANSPORT_H
#define FAKE_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

#include "transport_curl.h"

enum { FAKE_TRANSPORT_MAX_POST_RESPONSES = 16, FAKE_TRANSPORT_MAX_REQUESTS = 16 };

typedef struct {
    const char* data;
    size_t len;
} fake_stream_chunk_t;

typedef struct {
    const char* expected_url;
    const char* const* expected_headers;
    size_t expected_headers_count;
    const char* expected_proxy_url;
    const char* expected_no_proxy;

    long status_get;
    long status_post;
    long status_stream;

    const char* response_get;
    size_t response_get_len;
    const char* response_post;
    size_t response_post_len;
    const char* post_responses[FAKE_TRANSPORT_MAX_POST_RESPONSES];
    size_t post_response_lens[FAKE_TRANSPORT_MAX_POST_RESPONSES];
    size_t post_responses_count;

    const char* stream_payload;
    size_t stream_payload_len;
    size_t stream_chunk_size;
    const fake_stream_chunk_t* stream_chunks;
    size_t stream_chunks_count;
    bool stream_use_scratch;
    char stream_scratch_fill;

    bool fail_get;
    bool fail_post;
    bool fail_stream;

    bool called_get;
    bool called_post;
    bool called_stream;
    bool headers_ok;
    bool proxy_ok;

    size_t get_calls;
    size_t post_calls;
    size_t stream_calls;
    size_t stream_cb_calls;

    char* last_body;
    size_t last_body_len;
    char* last_request_body;
    size_t last_request_len;
    char* request_bodies[FAKE_TRANSPORT_MAX_REQUESTS];
    size_t request_lens[FAKE_TRANSPORT_MAX_REQUESTS];
    size_t request_count;
} fake_transport_state_t;

fake_transport_state_t* fake_transport_state(void);
void fake_transport_reset(void);

#endif  // FAKE_TRANSPORT_H
