project('desi', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
    'buildtype=debugoptimized',
  ]
)

# Dependencies
cc = meson.get_compiler('c')

# libcurl
curl_dep = dependency('libcurl', required: true)

# jstok - header-only library in parent directory
jstok_inc = include_directories('../jstok')
jstok_dep = declare_dependency(include_directories: jstok_inc)

# Include directories
inc = include_directories('include')

# Library
sources = files(
  'src/llm.c',
  'src/jstok_impl.c',
  'src/transport_curl.c',
  'src/json_core.c',
  'src/json_build.c',
  'src/protocol_chat.c',
  'src/protocol_completions.c',
  'src/protocol_embeddings.c',
  'src/sse.c',
  'src/tools_accum.c',
  'src/tools_loop.c',
)

libdesi = library('desi',
  sources,
  include_directories: inc,
  dependencies: [curl_dep, jstok_dep],
  install: true,
)

# Public header
install_headers('include/llm/llm.h', subdir: 'llm')

# pkg-config
pkg = import('pkgconfig')
pkg.generate(libdesi,
  name: 'desi',
  description: 'Minimal C LLM client',
  version: meson.project_version(),
)

# Tests
if get_option('tests')
  test_transport = executable('test_transport',
    'tests/test_transport.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('transport', test_transport)

  test_sse_limits = executable('test_sse_limits',
    'tests/test_sse_limits.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('sse_limits', test_sse_limits)

  test_tls = executable('test_tls',
    'tests/test_tls.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('tls', test_tls)

  test_json_build = executable('test_json_build',
    'tests/test_json_build.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('json_build', test_json_build)

  test_tool_calls_build = executable('test_tool_calls_build',
    'tests/test_tool_calls_build.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('tool_calls_build', test_tool_calls_build)

  test_tool_message_helpers = executable('test_tool_message_helpers',
    'tests/test_tool_message_helpers.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('tool_message_helpers', test_tool_message_helpers)

  test_tools_accum = executable('test_tools_accum',
    'tests/test_tools_accum.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('tools_accum', test_tools_accum)

  test_tool_deltas = executable('test_tool_deltas',
    'tests/test_tool_delta_callbacks.c',
    'tests/fake_transport.c',
    'src/llm.c',
    'src/jstok_impl.c',
    'src/json_core.c',
    'src/json_build.c',
    'src/protocol_chat.c',
    'src/protocol_completions.c',
    'src/protocol_embeddings.c',
    'src/sse.c',
    'src/tools_accum.c',
    'src/tools_loop.c',
    include_directories: [inc, include_directories('src'), include_directories('tests')],
    dependencies: [jstok_dep],
    install: false,
  )
  test('tool_deltas', test_tool_deltas)

  test_stream_usage = executable('test_stream_usage',
    'tests/test_stream_usage.c',
    'tests/fake_transport.c',
    'src/llm.c',
    'src/jstok_impl.c',
    'src/json_core.c',
    'src/json_build.c',
    'src/protocol_chat.c',
    'src/protocol_completions.c',
    'src/protocol_embeddings.c',
    'src/sse.c',
    'src/tools_accum.c',
    'src/tools_loop.c',
    include_directories: [inc, include_directories('src'), include_directories('tests')],
    dependencies: [jstok_dep],
    install: false,
  )
  test('stream_usage', test_stream_usage)

  test_auth = executable('test_auth',
    'tests/test_auth.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('auth', test_auth)

  test_proxy = executable('test_proxy',
    'tests/test_proxy.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('proxy', test_proxy)

  test_live = executable('test_live',
    'tests/test_live.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('live_basic', test_live, suite: 'live')

  test_transport_contract = executable('test_transport_contract',
    'tests/test_transport_contract.c',
    'tests/fake_transport.c',
    'src/llm.c',
    'src/jstok_impl.c',
    'src/json_core.c',
    'src/json_build.c',
    'src/protocol_chat.c',
    'src/protocol_completions.c',
    'src/protocol_embeddings.c',
    'src/sse.c',
    'src/tools_accum.c',
    'src/tools_loop.c',
    include_directories: [inc, include_directories('src'), include_directories('tests')],
    dependencies: [jstok_dep],
    install: false,
  )
  test('transport_contract', test_transport_contract)

  test_error_detail = executable('test_error_detail',
    'tests/test_error_detail.c',
    'tests/fake_transport.c',
    'src/llm.c',
    'src/jstok_impl.c',
    'src/json_core.c',
    'src/json_build.c',
    'src/protocol_chat.c',
    'src/protocol_completions.c',
    'src/protocol_embeddings.c',
    'src/sse.c',
    'src/tools_accum.c',
    'src/tools_loop.c',
    include_directories: [inc, include_directories('src'), include_directories('tests')],
    dependencies: [jstok_dep],
    install: false,
  )
  test('error_detail', test_error_detail)

  test_request_opts = executable('test_request_opts',
    'tests/test_request_opts.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('request_opts', test_request_opts)

  test_errstr = executable('test_errstr',
    'tests/test_errstr.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('errstr', test_errstr)

  test_last_error = executable('test_last_error',
    'tests/test_last_error.c',
    'tests/fake_transport.c',
    'src/llm.c',
    'src/jstok_impl.c',
    'src/json_core.c',
    'src/json_build.c',
    'src/protocol_chat.c',
    'src/protocol_completions.c',
    'src/protocol_embeddings.c',
    'src/sse.c',
    'src/tools_accum.c',
    'src/tools_loop.c',
    include_directories: [inc, include_directories('src'), include_directories('tests')],
    dependencies: [jstok_dep],
    install: false,
  )
  test('last_error', test_last_error)

  test_cancellation = executable('test_cancellation',
    'tests/test_cancellation.c',
    'tests/fake_transport.c',
    'src/llm.c',
    'src/jstok_impl.c',
    'src/json_core.c',
    'src/json_build.c',
    'src/protocol_chat.c',
    'src/protocol_completions.c',
    'src/protocol_embeddings.c',
    'src/sse.c',
    'src/tools_accum.c',
    'src/tools_loop.c',
    include_directories: [inc, include_directories('src'), include_directories('tests')],
    dependencies: [jstok_dep],
    install: false,
  )
  test('cancellation', test_cancellation)

  test_tool_loop_requests = executable('test_tool_loop_requests',
    'tests/test_tool_loop_requests.c',
    'tests/fake_transport.c',
    'src/llm.c',
    'src/jstok_impl.c',
    'src/json_core.c',
    'src/json_build.c',
    'src/protocol_chat.c',
    'src/protocol_completions.c',
    'src/protocol_embeddings.c',
    'src/sse.c',
    'src/tools_accum.c',
    'src/tools_loop.c',
    include_directories: [inc, include_directories('src'), include_directories('tests')],
    dependencies: [jstok_dep],
    install: false,
  )
  test('tool_loop_requests', test_tool_loop_requests)
  
  # llmctl is the main test runner/CLI tool
  llmctl = executable('llmctl',
    'tests/llmctl.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('live_llmctl', llmctl, suite: 'live')
endif

if get_option('fuzz')
  fuzz_cflags = ['-fsanitize=fuzzer', '-fno-omit-frame-pointer']
  fuzz_link_args = ['-fsanitize=fuzzer']
  fuzz_inc = [inc, include_directories('src')]

  executable('fuzz_sse_scanner',
    'tests/fuzz_sse_scanner.c',
    'src/sse.c',
    include_directories: fuzz_inc,
    dependencies: [jstok_dep],
    c_args: fuzz_cflags,
    link_args: fuzz_link_args,
    install: false,
  )

  executable('fuzz_sse_fragmented',
    'tests/fuzz_sse_fragmented.c',
    'src/sse.c',
    include_directories: fuzz_inc,
    dependencies: [jstok_dep],
    c_args: fuzz_cflags,
    link_args: fuzz_link_args,
    install: false,
  )

  executable('fuzz_sse_config',
    'tests/fuzz_sse_config.c',
    'src/sse.c',
    include_directories: fuzz_inc,
    dependencies: [jstok_dep],
    c_args: fuzz_cflags,
    link_args: fuzz_link_args,
    install: false,
  )

  executable('fuzz_json_spans',
    'tests/fuzz_json_spans.c',
    'src/jstok_impl.c',
    'src/json_core.c',
    include_directories: fuzz_inc,
    dependencies: [jstok_dep],
    c_args: fuzz_cflags,
    link_args: fuzz_link_args,
    install: false,
  )

  executable('fuzz_tool_accum',
    'tests/fuzz_tool_accum.c',
    'src/tools_accum.c',
    include_directories: fuzz_inc,
    dependencies: [jstok_dep],
    c_args: fuzz_cflags,
    link_args: fuzz_link_args,
    install: false,
  )
endif

# Examples
if get_option('examples')
  chat_simple = executable('chat_simple',
    'examples/chat_simple.c',
    include_directories: inc,
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  chat_stream = executable('chat_stream',
    'examples/chat_stream.c',
    include_directories: inc,
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  chat_advanced = executable('chat_advanced',
    'examples/chat_advanced.c',
    include_directories: inc,
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  tool_loop = executable('tool_loop',
    'examples/tool_loop.c',
    include_directories: inc,
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
endif
