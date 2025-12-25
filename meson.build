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

  test_tools_accum = executable('test_tools_accum',
    'tests/test_tools_accum.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  test('tools_accum', test_tools_accum)

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
  
  # llmctl is the main test runner/CLI tool
  llmctl = executable('llmctl',
    'tests/llmctl.c',
    include_directories: [inc, include_directories('src')],
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
  # We do not add test('llmctl', ...) here if it requires a server. 
  # But usually test runners mock things. Since this is an integration test suite now,
  # we might want to keep it manual or make it skip if server is down.
  # For now, I'll leave it as a target but not a default meson test if it depends on external server.
  # Wait, the previous meson.build had test('llmctl', llmctl). 
  # If llmctl.c is now the smoke test, it fails without server.
  # I will NOT add it to test() to avoid breaking `meson test` in CI without server.
  # The user can run it manually with ./build/test_live
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
  tool_loop = executable('tool_loop',
    'examples/tool_loop.c',
    include_directories: inc,
    dependencies: [curl_dep, jstok_dep],
    link_with: libdesi,
    install: false,
  )
endif
