#ifndef LLM_H
#define LLM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque client handle
typedef struct llm_client llm_client_t;

// Timeout configuration
typedef struct {
    long connect_timeout_ms;
    long overall_timeout_ms;
    long read_idle_timeout_ms;  // for streaming
} llm_timeout_t;

// Limits configuration
typedef struct {
    size_t max_response_bytes;
    size_t max_line_bytes;        // SSE line cap
    size_t max_frame_bytes;       // SSE data payload cap
    size_t max_sse_buffer_bytes;  // SSE buffer cap
    size_t max_tool_args_bytes_per_call;
    size_t max_tool_args_bytes_per_turn;
    size_t max_tool_output_bytes_total;
    size_t max_embedding_input_bytes;
    size_t max_embedding_inputs;
    size_t max_content_parts;  // content[] part count cap
    size_t max_content_bytes;  // content[] raw JSON bytes cap
} llm_limits_t;

typedef struct {
    bool has_temperature;
    double temperature;
    bool has_top_p;
    double top_p;
    bool has_max_tokens;
    int64_t max_tokens;
    const char* stop;  // optional single stop string
    size_t stop_len;
    const char* const* stop_list;  // optional array of stop strings
    const size_t* stop_lens;
    size_t stop_count;
    bool has_frequency_penalty;
    double frequency_penalty;
    bool has_presence_penalty;
    double presence_penalty;
    bool has_seed;
    int64_t seed;
} llm_request_opts_t;

typedef enum {
    LLM_TLS_VERIFY_DEFAULT = 0,
    LLM_TLS_VERIFY_ON,
    LLM_TLS_VERIFY_OFF,
} llm_tls_verify_mode_t;

typedef bool (*llm_tls_key_password_cb)(void* user_data, char* out, size_t out_cap);

// TLS configuration (copied into the client)
typedef struct llm_tls_config {
    const char* ca_bundle_path;               // optional path to PEM bundle
    const char* ca_dir_path;                  // optional path to CA directory
    const char* client_cert_path;             // optional path to client cert PEM
    const char* client_key_path;              // optional path to client key PEM
    llm_tls_key_password_cb key_password_cb;  // optional key password callback
    void* key_password_user_data;
    llm_tls_verify_mode_t verify_peer;
    llm_tls_verify_mode_t verify_host;
    bool insecure;  // explicitly disable verification
} llm_tls_config_t;

// Client creation options (opt-in behaviors).
typedef struct {
    bool enable_last_error;
} llm_client_init_opts_t;

// Model identifier
typedef struct {
    const char* name;
} llm_model_t;

// Finish reason enum
typedef enum {
    LLM_FINISH_REASON_STOP,
    LLM_FINISH_REASON_LENGTH,
    LLM_FINISH_REASON_TOOL_CALLS,
    LLM_FINISH_REASON_CONTENT_FILTER,
    LLM_FINISH_REASON_UNKNOWN,
} llm_finish_reason_t;

typedef enum {
    LLM_ERR_NONE = 0,
    LLM_ERR_CANCELLED,
    LLM_ERR_FAILED,
} llm_error_t;

typedef enum {
    LLM_ERROR_STAGE_NONE = 0,
    LLM_ERROR_STAGE_TRANSPORT,
    LLM_ERROR_STAGE_TLS,
    LLM_ERROR_STAGE_SSE,
    LLM_ERROR_STAGE_JSON,
    LLM_ERROR_STAGE_PROTOCOL,
} llm_error_stage_t;

typedef struct {
    llm_error_t code;
    llm_error_stage_t stage;
    long http_status;
    bool has_http_status;
    const char* message;
    size_t message_len;
    const char* type;
    size_t type_len;
    const char* error_code;
    size_t error_code_len;
    const char* raw_body;
    size_t raw_body_len;
    void* _internal;  // Internal buffer for raw error body
} llm_error_detail_t;

// Message role
typedef enum {
    LLM_ROLE_SYSTEM,
    LLM_ROLE_USER,
    LLM_ROLE_ASSISTANT,
    LLM_ROLE_TOOL,
} llm_role_t;

// Message structure (span-based)
typedef struct {
    llm_role_t role;
    const char* content;  // optional, may be NULL
    size_t content_len;
    // For tool calls
    const char* tool_call_id;  // optional
    size_t tool_call_id_len;
    const char* name;  // optional (for tool role)
    size_t name_len;
    // For assistant tool calls (raw JSON array)
    const char* tool_calls_json;  // optional
    size_t tool_calls_json_len;
    // For content[] raw JSON array (mutually exclusive with content)
    const char* content_json;  // optional
    size_t content_json_len;
} llm_message_t;

// Helper for tool-result messages; caller owns all spans (no allocation).
bool llm_tool_message_init(llm_message_t* msg, const char* content, size_t content_len, const char* tool_call_id,
                           size_t tool_call_id_len, const char* tool_name, size_t tool_name_len);

// Tool call (span-based)
typedef struct {
    const char* id;
    size_t id_len;
    const char* name;
    size_t name_len;
    const char* arguments;  // JSON string
    size_t arguments_len;
} llm_tool_call_t;

typedef struct {
    const char* id;  // optional
    size_t id_len;
    const char* name;  // required
    size_t name_len;
    const char* arguments_json;  // required raw JSON
    size_t arguments_json_len;
} llm_tool_call_build_t;

// Writes tool_calls JSON array for assistant messages into caller buffer.
// arguments_json is escaped into a JSON string; out_len receives bytes written.
bool llm_tool_calls_json_write(const llm_tool_call_build_t* calls, size_t calls_count, char* out, size_t out_cap,
                               size_t max_args_bytes_per_call, size_t* out_len);

// Writes request options as a JSON object string into caller buffer.
// out_len receives bytes written (excluding the terminating NUL).
bool llm_request_opts_json_write(const llm_request_opts_t* opts, char* out, size_t out_cap, size_t max_stop_strings,
                                 size_t max_stop_bytes, size_t* out_len);

// Chat completion choice (non-stream)
typedef struct {
    llm_finish_reason_t finish_reason;
    const char* content;  // may be NULL
    size_t content_len;
    const char* reasoning_content;  // may be NULL
    size_t reasoning_content_len;
    llm_tool_call_t* tool_calls;  // array
    size_t tool_calls_count;
    const char* tool_calls_json;  // raw JSON array
    size_t tool_calls_json_len;
} llm_chat_choice_t;

// Chat completion result (non-stream)
typedef struct {
    llm_chat_choice_t* choices;  // array
    size_t choices_count;

    // Convenience aliases for choices[0]
    llm_finish_reason_t finish_reason;
    const char* content;  // may be NULL
    size_t content_len;
    const char* reasoning_content;  // may be NULL
    size_t reasoning_content_len;
    llm_tool_call_t* tool_calls;  // array
    size_t tool_calls_count;
    const char* tool_calls_json;  // raw JSON array
    size_t tool_calls_json_len;
    void* _internal;  // Internal buffer for spans
} llm_chat_result_t;

// Tool call delta (streaming partial)
typedef struct {
    size_t index;
    const char* id;  // optional, may be NULL
    size_t id_len;
    const char* name;  // optional
    size_t name_len;
    const char* arguments_fragment;  // optional
    size_t arguments_fragment_len;
} llm_tool_call_delta_t;

// Chat chunk delta (streaming)
typedef struct {
    const char* content_delta;  // optional
    size_t content_delta_len;
    const char* reasoning_delta;  // optional
    size_t reasoning_delta_len;
    llm_tool_call_delta_t* tool_call_deltas;  // array
    size_t tool_call_deltas_count;
    llm_finish_reason_t finish_reason;  // may be UNKNOWN
} llm_chat_chunk_delta_t;

typedef struct {
    size_t prompt_tokens;
    size_t completion_tokens;
    size_t total_tokens;
    bool has_prompt_tokens;
    bool has_completion_tokens;
    bool has_total_tokens;
} llm_usage_t;

// Streaming callbacks
typedef struct {
    void* user_data;
    void (*on_content_delta)(void* user_data, const char* delta, size_t len);
    void (*on_reasoning_delta)(void* user_data, const char* delta, size_t len);
    void (*on_tool_args_fragment)(void* user_data, size_t tool_index, const char* fragment, size_t len);
    void (*on_tool_call_delta)(void* user_data, const llm_tool_call_delta_t* delta);
    void (*on_tool_args_complete)(void* user_data, size_t tool_index, const char* args_json, size_t len);
    void (*on_usage)(void* user_data, const llm_usage_t* usage);
    void (*on_finish_reason)(void* user_data, llm_finish_reason_t reason);
    bool include_usage;
} llm_stream_callbacks_t;

typedef bool (*llm_abort_cb)(void* user_data);

// Client creation and destruction
llm_client_t* llm_client_create_with_headers_opts(const char* base_url, const llm_model_t* model,
                                                  const llm_timeout_t* timeout, const llm_limits_t* limits,
                                                  const char* const* headers, size_t headers_count,
                                                  const llm_client_init_opts_t* opts);
llm_client_t* llm_client_create_opts(const char* base_url, const llm_model_t* model, const llm_timeout_t* timeout,
                                     const llm_limits_t* limits, const llm_client_init_opts_t* opts);
llm_client_t* llm_client_create(const char* base_url, const llm_model_t* model, const llm_timeout_t* timeout,
                                const llm_limits_t* limits);
// Copies headers into the client. Each entry must be a complete "Header: value" string.
llm_client_t* llm_client_create_with_headers(const char* base_url, const llm_model_t* model,
                                             const llm_timeout_t* timeout, const llm_limits_t* limits,
                                             const char* const* headers, size_t headers_count);
void llm_client_destroy(llm_client_t* client);
// Copies model name into the client; caller must synchronize with in-flight requests.
bool llm_client_set_model(llm_client_t* client, const llm_model_t* model);
// Copies api_key into a per-client Authorization header. Pass NULL to clear.
bool llm_client_set_api_key(llm_client_t* client, const char* api_key);
// Copies TLS config into the client. Pass NULL to clear.
bool llm_client_set_tls_config(llm_client_t* client, const llm_tls_config_t* tls);
// Copies proxy URL into the client. Pass NULL or empty to clear.
bool llm_client_set_proxy(llm_client_t* client, const char* proxy_url);
// Copies no-proxy list into the client. Pass NULL or empty to clear.
bool llm_client_set_no_proxy(llm_client_t* client, const char* no_proxy_list);
// Returns NULL unless last-error storage was enabled at client creation.
// The pointer is owned by the client and cleared at the start of each request.
// Not thread-safe with concurrent requests on the same client.
const llm_error_detail_t* llm_client_last_error(const llm_client_t* client);

// Error detail lifetime: free any owned raw body buffer.
void llm_error_detail_free(llm_error_detail_t* detail);
// Returns a stable, non-formatted string for an error code.
const char* llm_errstr(llm_error_t code);

// Health check
bool llm_health(llm_client_t* client);
// Per-request headers are not copied; entries override client defaults with the same name (case-insensitive).
bool llm_health_with_headers(llm_client_t* client, const char* const* headers, size_t headers_count);
llm_error_t llm_health_ex(llm_client_t* client, llm_error_detail_t* detail);
llm_error_t llm_health_with_headers_ex(llm_client_t* client, const char* const* headers, size_t headers_count,
                                       llm_error_detail_t* detail);

// Models list (simple string array)
char** llm_models_list(llm_client_t* client, size_t* count);
char** llm_models_list_with_headers(llm_client_t* client, size_t* count, const char* const* headers,
                                    size_t headers_count);
llm_error_t llm_models_list_ex(llm_client_t* client, char*** models, size_t* count, llm_error_detail_t* detail);
llm_error_t llm_models_list_with_headers_ex(llm_client_t* client, char*** models, size_t* count,
                                            const char* const* headers, size_t headers_count,
                                            llm_error_detail_t* detail);
void llm_models_list_free(char** models, size_t count);

// Properties (server info)
// Returns JSON span (caller can parse)
bool llm_props_get(llm_client_t* client, const char** json, size_t* len);
bool llm_props_get_with_headers(llm_client_t* client, const char** json, size_t* len, const char* const* headers,
                                size_t headers_count);
llm_error_t llm_props_get_ex(llm_client_t* client, const char** json, size_t* len, llm_error_detail_t* detail);
llm_error_t llm_props_get_with_headers_ex(llm_client_t* client, const char** json, size_t* len,
                                          const char* const* headers, size_t headers_count, llm_error_detail_t* detail);

// Completion choice (span-based)
typedef struct {
    const char* text;
    size_t text_len;
} llm_completion_choice_t;

// Completions result (non-stream)
typedef struct {
    llm_completion_choice_t* choices;  // array
    size_t choices_count;
    void* _internal;  // Internal buffer for spans
} llm_completions_result_t;

typedef struct {
    const char* text;
    size_t text_len;
} llm_embedding_input_t;

typedef struct {
    const char* embedding;
    size_t embedding_len;
} llm_embedding_item_t;

typedef struct {
    llm_embedding_item_t* data;  // array
    size_t data_count;
    void* _internal;  // Internal buffer for spans
} llm_embeddings_result_t;

// Completions (non-stream)
// Returns text spans into the response buffer
bool llm_completions(llm_client_t* client, const char* prompt, size_t prompt_len,
                     const char* params_json,  // optional JSON string
                     llm_completions_result_t* result);
bool llm_completions_with_headers(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                                  llm_completions_result_t* result, const char* const* headers, size_t headers_count);
llm_error_t llm_completions_ex(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                               llm_completions_result_t* result, llm_error_detail_t* detail);
llm_error_t llm_completions_with_headers_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                            const char* params_json, llm_completions_result_t* result,
                                            const char* const* headers, size_t headers_count,
                                            llm_error_detail_t* detail);
void llm_completions_free(llm_completions_result_t* result);

// Completions (stream)
bool llm_completions_stream(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                            const llm_stream_callbacks_t* callbacks);
bool llm_completions_stream_with_headers(llm_client_t* client, const char* prompt, size_t prompt_len,
                                         const char* params_json, const llm_stream_callbacks_t* callbacks,
                                         const char* const* headers, size_t headers_count);
bool llm_completions_stream_choice(llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json,
                                   size_t choice_index, const llm_stream_callbacks_t* callbacks);
bool llm_completions_stream_choice_with_headers(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                const char* params_json, size_t choice_index,
                                                const llm_stream_callbacks_t* callbacks, const char* const* headers,
                                                size_t headers_count);
llm_error_t llm_completions_stream_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                      const char* params_json, const llm_stream_callbacks_t* callbacks,
                                      llm_abort_cb abort_cb, void* abort_user_data);
llm_error_t llm_completions_stream_with_headers_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                   const char* params_json, const llm_stream_callbacks_t* callbacks,
                                                   llm_abort_cb abort_cb, void* abort_user_data,
                                                   const char* const* headers, size_t headers_count);
llm_error_t llm_completions_stream_choice_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                             const char* params_json, size_t choice_index,
                                             const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb,
                                             void* abort_user_data);
llm_error_t llm_completions_stream_choice_with_headers_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                          const char* params_json, size_t choice_index,
                                                          const llm_stream_callbacks_t* callbacks,
                                                          llm_abort_cb abort_cb, void* abort_user_data,
                                                          const char* const* headers, size_t headers_count);
llm_error_t llm_completions_stream_detail_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                             const char* params_json, const llm_stream_callbacks_t* callbacks,
                                             llm_abort_cb abort_cb, void* abort_user_data, llm_error_detail_t* detail);
llm_error_t llm_completions_stream_with_headers_detail_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                          const char* params_json,
                                                          const llm_stream_callbacks_t* callbacks,
                                                          llm_abort_cb abort_cb, void* abort_user_data,
                                                          const char* const* headers, size_t headers_count,
                                                          llm_error_detail_t* detail);
llm_error_t llm_completions_stream_choice_detail_ex(llm_client_t* client, const char* prompt, size_t prompt_len,
                                                    const char* params_json, size_t choice_index,
                                                    const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb,
                                                    void* abort_user_data, llm_error_detail_t* detail);
llm_error_t llm_completions_stream_choice_with_headers_detail_ex(
    llm_client_t* client, const char* prompt, size_t prompt_len, const char* params_json, size_t choice_index,
    const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb, void* abort_user_data, const char* const* headers,
    size_t headers_count, llm_error_detail_t* detail);

// Embeddings (non-stream)
// Returns embedding spans into the response buffer
bool llm_embeddings(llm_client_t* client, const llm_embedding_input_t* inputs, size_t inputs_count,
                    const char* params_json, llm_embeddings_result_t* result);
bool llm_embeddings_with_headers(llm_client_t* client, const llm_embedding_input_t* inputs, size_t inputs_count,
                                 const char* params_json, llm_embeddings_result_t* result, const char* const* headers,
                                 size_t headers_count);
llm_error_t llm_embeddings_ex(llm_client_t* client, const llm_embedding_input_t* inputs, size_t inputs_count,
                              const char* params_json, llm_embeddings_result_t* result, llm_error_detail_t* detail);
llm_error_t llm_embeddings_with_headers_ex(llm_client_t* client, const llm_embedding_input_t* inputs,
                                           size_t inputs_count, const char* params_json,
                                           llm_embeddings_result_t* result, const char* const* headers,
                                           size_t headers_count, llm_error_detail_t* detail);
void llm_embeddings_free(llm_embeddings_result_t* result);

// Chat non-stream
bool llm_chat(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
              const char* params_json,           // optional
              const char* tooling_json,          // optional
              const char* response_format_json,  // optional
              llm_chat_result_t* result);
bool llm_chat_with_headers(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                           const char* params_json, const char* tooling_json, const char* response_format_json,
                           llm_chat_result_t* result, const char* const* headers, size_t headers_count);
llm_error_t llm_chat_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                        const char* params_json, const char* tooling_json, const char* response_format_json,
                        llm_chat_result_t* result, llm_error_detail_t* detail);
llm_error_t llm_chat_with_headers_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                     const char* params_json, const char* tooling_json,
                                     const char* response_format_json, llm_chat_result_t* result,
                                     const char* const* headers, size_t headers_count, llm_error_detail_t* detail);
void llm_chat_result_free(llm_chat_result_t* result);
bool llm_chat_choice_get(const llm_chat_result_t* result, size_t index, const llm_chat_choice_t** out_choice);
bool llm_completions_choice_get(const llm_completions_result_t* result, size_t index,
                                const llm_completion_choice_t** out_choice);

// Chat stream
bool llm_chat_stream(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                     const char* params_json, const char* tooling_json, const char* response_format_json,
                     const llm_stream_callbacks_t* callbacks);
bool llm_chat_stream_with_headers(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                  const char* params_json, const char* tooling_json, const char* response_format_json,
                                  const llm_stream_callbacks_t* callbacks, const char* const* headers,
                                  size_t headers_count);
bool llm_chat_stream_choice(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                            const char* params_json, const char* tooling_json, const char* response_format_json,
                            size_t choice_index, const llm_stream_callbacks_t* callbacks);
bool llm_chat_stream_choice_with_headers(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                         const char* params_json, const char* tooling_json,
                                         const char* response_format_json, size_t choice_index,
                                         const llm_stream_callbacks_t* callbacks, const char* const* headers,
                                         size_t headers_count);
llm_error_t llm_chat_stream_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                               const char* params_json, const char* tooling_json, const char* response_format_json,
                               const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb, void* abort_user_data);
llm_error_t llm_chat_stream_with_headers_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                            const char* params_json, const char* tooling_json,
                                            const char* response_format_json, const llm_stream_callbacks_t* callbacks,
                                            llm_abort_cb abort_cb, void* abort_user_data, const char* const* headers,
                                            size_t headers_count);
llm_error_t llm_chat_stream_choice_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                      const char* params_json, const char* tooling_json,
                                      const char* response_format_json, size_t choice_index,
                                      const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb,
                                      void* abort_user_data);
llm_error_t llm_chat_stream_choice_with_headers_ex(llm_client_t* client, const llm_message_t* messages,
                                                   size_t messages_count, const char* params_json,
                                                   const char* tooling_json, const char* response_format_json,
                                                   size_t choice_index, const llm_stream_callbacks_t* callbacks,
                                                   llm_abort_cb abort_cb, void* abort_user_data,
                                                   const char* const* headers, size_t headers_count);
llm_error_t llm_chat_stream_detail_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                      const char* params_json, const char* tooling_json,
                                      const char* response_format_json, const llm_stream_callbacks_t* callbacks,
                                      llm_abort_cb abort_cb, void* abort_user_data, llm_error_detail_t* detail);
llm_error_t llm_chat_stream_with_headers_detail_ex(llm_client_t* client, const llm_message_t* messages,
                                                   size_t messages_count, const char* params_json,
                                                   const char* tooling_json, const char* response_format_json,
                                                   const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb,
                                                   void* abort_user_data, const char* const* headers,
                                                   size_t headers_count, llm_error_detail_t* detail);
llm_error_t llm_chat_stream_choice_detail_ex(llm_client_t* client, const llm_message_t* messages, size_t messages_count,
                                             const char* params_json, const char* tooling_json,
                                             const char* response_format_json, size_t choice_index,
                                             const llm_stream_callbacks_t* callbacks, llm_abort_cb abort_cb,
                                             void* abort_user_data, llm_error_detail_t* detail);
llm_error_t llm_chat_stream_choice_with_headers_detail_ex(llm_client_t* client, const llm_message_t* messages,
                                                          size_t messages_count, const char* params_json,
                                                          const char* tooling_json, const char* response_format_json,
                                                          size_t choice_index, const llm_stream_callbacks_t* callbacks,
                                                          llm_abort_cb abort_cb, void* abort_user_data,
                                                          const char* const* headers, size_t headers_count,
                                                          llm_error_detail_t* detail);

// Tool loop runner
typedef bool (*llm_tool_dispatch_cb)(void* user_data, const char* tool_name, size_t name_len, const char* args_json,
                                     size_t args_len, char** result_json, size_t* result_len);

bool llm_tool_loop_run(llm_client_t* client, const llm_message_t* initial_messages, size_t initial_count,
                       const char* params_json, const char* tooling_json, const char* response_format_json,
                       llm_tool_dispatch_cb dispatch, void* dispatch_user_data, size_t max_turns);
bool llm_tool_loop_run_with_headers(llm_client_t* client, const llm_message_t* initial_messages, size_t initial_count,
                                    const char* params_json, const char* tooling_json, const char* response_format_json,
                                    llm_tool_dispatch_cb dispatch, void* dispatch_user_data, size_t max_turns,
                                    const char* const* headers, size_t headers_count);
llm_error_t llm_tool_loop_run_ex(llm_client_t* client, const llm_message_t* initial_messages, size_t initial_count,
                                 const char* params_json, const char* tooling_json, const char* response_format_json,
                                 llm_tool_dispatch_cb dispatch, void* dispatch_user_data, llm_abort_cb abort_cb,
                                 void* abort_user_data, size_t max_turns);
llm_error_t llm_tool_loop_run_with_headers_ex(llm_client_t* client, const llm_message_t* initial_messages,
                                              size_t initial_count, const char* params_json, const char* tooling_json,
                                              const char* response_format_json, llm_tool_dispatch_cb dispatch,
                                              void* dispatch_user_data, llm_abort_cb abort_cb, void* abort_user_data,
                                              size_t max_turns, const char* const* headers, size_t headers_count);

// Utility functions
llm_finish_reason_t llm_finish_reason_from_string(const char* str, size_t len);
const char* llm_finish_reason_to_string(llm_finish_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif  // LLM_H
