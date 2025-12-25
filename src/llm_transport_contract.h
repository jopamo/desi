#ifndef LLM_TRANSPORT_CONTRACT_H
#define LLM_TRANSPORT_CONTRACT_H

/*
Transport contract for llm transport backends.

The transport layer is a byte pump. It must not parse JSON or interpret protocol state.

Ownership and lifetime (http_get/http_post):
- On success, the transport allocates a response body buffer with a malloc-compatible allocator,
  sets *body and *len, and transfers ownership to the caller.
- The response buffer remains valid until the caller frees it.
- The transport must not retain or free the buffer after returning.
- The buffer is not required to be NUL-terminated; if a terminator is added, it is not counted in *len.
- On failure, the transport sets *body = NULL, *len = 0, and frees any internal buffers.

Headers:
- headers and headers[i] are read-only.
- The transport may read headers during the call, including while invoking streaming callbacks.
- The transport must not mutate headers or retain header pointers after returning.

Streaming (http_post_stream):
- stream_cb is invoked synchronously on the caller thread.
- Callbacks are serialized and non-reentrant.
- No callbacks occur after http_post_stream returns.
- The chunk pointer is valid only for the duration of the callback; callers must copy to retain data.

Failure propagation:
- Any transport, TLS, or size-cap error returns false.
- Streaming must stop on failure and emit no further callbacks.
- If a backend can detect callback failure, it must propagate that as a false return.
*/

#endif  // LLM_TRANSPORT_CONTRACT_H
