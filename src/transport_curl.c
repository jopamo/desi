#include "transport_curl.h"

#include <curl/curl.h>

#include "llm/internal.h"
#include "llm/llm.h"

struct write_ctx {
    struct growbuf* buf;
    size_t max_bytes;
};

static struct curl_slist* append_headers(struct curl_slist* list, const char* const* headers, size_t headers_count) {
    for (size_t i = 0; i < headers_count; i++) {
        list = curl_slist_append(list, headers[i]);
    }
    return list;
}

static bool resolve_verify_mode(llm_tls_verify_mode_t mode, bool default_value) {
    switch (mode) {
        case LLM_TLS_VERIFY_OFF:
            return false;
        case LLM_TLS_VERIFY_ON:
            return true;
        case LLM_TLS_VERIFY_DEFAULT:
        default:
            return default_value;
    }
}

static bool apply_tls_config(CURL* curl, const llm_tls_config_t* tls, char* key_pass_buf, size_t key_pass_cap) {
    bool verify_peer = true;
    bool verify_host = true;
    const char* ca_bundle_path = NULL;
    const char* ca_dir_path = NULL;
    const char* client_cert_path = NULL;
    const char* client_key_path = NULL;
    bool insecure = false;

    if (tls) {
        verify_peer = resolve_verify_mode(tls->verify_peer, verify_peer);
        verify_host = resolve_verify_mode(tls->verify_host, verify_host);
        ca_bundle_path = tls->ca_bundle_path;
        ca_dir_path = tls->ca_dir_path;
        client_cert_path = tls->client_cert_path;
        client_key_path = tls->client_key_path;
        insecure = tls->insecure;
    }

    if (insecure) {
        verify_peer = false;
        verify_host = false;
    }

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_peer ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_host ? 2L : 0L);

    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (ca_bundle_path) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle_path);
    } else if (info && info->cainfo && info->cainfo[0]) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, info->cainfo);
    }

    if (ca_dir_path) {
        curl_easy_setopt(curl, CURLOPT_CAPATH, ca_dir_path);
    } else if (info && info->capath && info->capath[0]) {
        curl_easy_setopt(curl, CURLOPT_CAPATH, info->capath);
    }

    if (client_cert_path) {
        curl_easy_setopt(curl, CURLOPT_SSLCERT, client_cert_path);
    }

    if (client_key_path) {
        curl_easy_setopt(curl, CURLOPT_SSLKEY, client_key_path);
    }

    if (tls && tls->key_password_cb) {
        if (!key_pass_buf || key_pass_cap == 0) return false;
        memset(key_pass_buf, 0, key_pass_cap);
        if (!tls->key_password_cb(tls->key_password_user_data, key_pass_buf, key_pass_cap)) {
            return false;
        }
        if (!memchr(key_pass_buf, '\0', key_pass_cap)) {
            return false;
        }
        curl_easy_setopt(curl, CURLOPT_KEYPASSWD, key_pass_buf);
    }

    return true;
}

static void apply_proxy_config(CURL* curl, const char* proxy_url) {
    if (proxy_url && proxy_url[0]) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url);
    } else {
        // Empty string disables libcurl environment proxy lookup.
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
    }
}

static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t realsize = size * nmemb;
    struct write_ctx* ctx = (struct write_ctx*)userdata;
    if (!growbuf_append(ctx->buf, ptr, realsize, ctx->max_bytes)) {
        return 0;  // Signal error to curl
    }
    return realsize;
}

bool http_get(const char* url, long timeout_ms, size_t max_response_bytes, const char* const* headers,
              size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url, char** body, size_t* len) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct growbuf buf;
    growbuf_init(&buf, 4096);
    struct write_ctx ctx = {&buf, max_response_bytes};

    struct curl_slist* header_list = NULL;
    header_list = append_headers(header_list, headers, headers_count);
    char key_pass_buf[1024];

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (!apply_tls_config(curl, tls, key_pass_buf, sizeof(key_pass_buf))) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        growbuf_free(&buf);
        memset(key_pass_buf, 0, sizeof(key_pass_buf));
        return false;
    }
    apply_proxy_config(curl, proxy_url);
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }
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

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    memset(key_pass_buf, 0, sizeof(key_pass_buf));
    return success;
}

bool http_post(const char* url, const char* json_body, long timeout_ms, size_t max_response_bytes,
               const char* const* headers, size_t headers_count, const llm_tls_config_t* tls, const char* proxy_url,
               char** body, size_t* len) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct growbuf buf;
    growbuf_init(&buf, 4096);
    struct write_ctx ctx = {&buf, max_response_bytes};

    struct curl_slist* header_list = NULL;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    header_list = append_headers(header_list, headers, headers_count);
    char key_pass_buf[1024];

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (!apply_tls_config(curl, tls, key_pass_buf, sizeof(key_pass_buf))) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        growbuf_free(&buf);
        memset(key_pass_buf, 0, sizeof(key_pass_buf));
        return false;
    }
    apply_proxy_config(curl, proxy_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
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

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    memset(key_pass_buf, 0, sizeof(key_pass_buf));
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

bool http_post_stream(const char* url, const char* json_body, long timeout_ms, long read_idle_timeout_ms,
                      const char* const* headers, size_t headers_count, const llm_tls_config_t* tls,
                      const char* proxy_url, stream_cb cb, void* user_data) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* header_list = NULL;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    header_list = append_headers(header_list, headers, headers_count);
    char key_pass_buf[1024];

    struct {
        stream_cb cb;
        void* user_data;
    } ctx = {cb, user_data};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (!apply_tls_config(curl, tls, key_pass_buf, sizeof(key_pass_buf))) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        memset(key_pass_buf, 0, sizeof(key_pass_buf));
        return false;
    }
    apply_proxy_config(curl, proxy_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
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

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    memset(key_pass_buf, 0, sizeof(key_pass_buf));
    return success;
}
