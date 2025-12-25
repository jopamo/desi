#include "transport_curl.h"

#include <curl/curl.h>

#include "llm/internal.h"
#include "llm/llm.h"

struct write_ctx {
    struct growbuf* buf;
    size_t max_bytes;
};

static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t realsize = size * nmemb;
    struct write_ctx* ctx = (struct write_ctx*)userdata;
    if (!growbuf_append(ctx->buf, ptr, realsize, ctx->max_bytes)) {
        return 0;  // Signal error to curl
    }
    return realsize;
}

bool http_get(const char* url, long timeout_ms, size_t max_response_bytes, char** body, size_t* len) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct growbuf buf;
    growbuf_init(&buf, 4096);
    struct write_ctx ctx = {&buf, max_response_bytes};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms > 10000 ? 10000 : timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    bool success = (res == CURLE_OK);

    if (success) {
        // Null terminate for convenience if there's space, but don't count it in len
        if (growbuf_append(&buf, "", 1, max_response_bytes + 1)) {
            buf.len--;  // don't count null
        }
        *body = buf.data;
        *len = buf.len;
    } else {
        growbuf_free(&buf);
        *body = NULL;
        *len = 0;
    }

    curl_easy_cleanup(curl);
    return success;
}

bool http_post(const char* url, const char* json_body, long timeout_ms, size_t max_response_bytes, char** body,
               size_t* len) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct growbuf buf;
    growbuf_init(&buf, 4096);
    struct write_ctx ctx = {&buf, max_response_bytes};

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms > 10000 ? 10000 : timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    bool success = (res == CURLE_OK);

    if (success) {
        if (growbuf_append(&buf, "", 1, max_response_bytes + 1)) {
            buf.len--;
        }
        *body = buf.data;
        *len = buf.len;
    } else {
        growbuf_free(&buf);
        *body = NULL;
        *len = 0;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return success;
}

static size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t realsize = size * nmemb;
    struct {
        stream_cb cb;
        void* user_data;
    }* ctx = userdata;
    ctx->cb(ptr, realsize, ctx->user_data);
    return realsize;
}

bool http_post_stream(const char* url, const char* json_body, long timeout_ms, long read_idle_timeout_ms, stream_cb cb,
                      void* user_data) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    struct {
        stream_cb cb;
        void* user_data;
    } ctx = {cb, user_data};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms > 10000 ? 10000 : timeout_ms);

    // For streaming, we might want to use CURLOPT_LOW_SPEED_LIMIT and CURLOPT_LOW_SPEED_TIME
    // to simulate read_idle_timeout_ms if curl doesn't have a direct "idle" timeout.
    if (read_idle_timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (read_idle_timeout_ms + 999) / 1000);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    bool success = (res == CURLE_OK);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return success;
}