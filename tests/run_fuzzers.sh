#!/bin/sh
set -eu

if ! command -v clang >/dev/null 2>&1; then
    echo "clang not found in PATH" >&2
    exit 1
fi
if ! command -v meson >/dev/null 2>&1; then
    echo "meson not found in PATH" >&2
    exit 1
fi

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$root_dir/build-fuzz"}
dict_dir="$root_dir/tests/fuzz_dicts"

mkdir -p "$dict_dir"

if [ ! -f "$dict_dir/sse.dict" ]; then
    cat >"$dict_dir/sse.dict" <<'EOF'
kw_data="data:"
kw_id="id:"
kw_event="event:"
kw_retry="retry:"
kw_comment=":"
kw_done="[DONE]"
sep_lf="\x0A"
sep_cr="\x0D"
sep_crlf="\x0D\x0A"
sep_space="\x20"
sample_frame="data: {}\x0A\x0A"
EOF
fi

if [ ! -f "$dict_dir/json_spans.dict" ]; then
    cat >"$dict_dir/json_spans.dict" <<'EOF'
obj_empty="{}"
arr_empty="[]"
lit_true="true"
lit_false="false"
lit_null="null"
num_zero="0"
num_one="1"
str_empty="\"\""
key_choices="\"choices\""
key_message="\"message\""
key_delta="\"delta\""
key_tool_calls="\"tool_calls\""
key_arguments="\"arguments\""
key_content="\"content\""
key_role="\"role\""
key_reasoning="\"reasoning\""
key_usage="\"usage\""
key_prompt_tokens="\"prompt_tokens\""
key_completion_tokens="\"completion_tokens\""
key_total_tokens="\"total_tokens\""
key_data="\"data\""
key_embedding="\"embedding\""
key_index="\"index\""
key_id="\"id\""
key_function="\"function\""
key_name="\"name\""
key_type="\"type\""
key_text="\"text\""
sample_choices="{\"choices\":[]}"
sample_tool="{\"tool_calls\":[{\"function\":{\"name\":\"tool\",\"arguments\":\"{}\"}}]}"
EOF
fi

if [ ! -f "$dict_dir/tool_accum.dict" ]; then
    cat >"$dict_dir/tool_accum.dict" <<'EOF'
frag_open="{"
frag_close="}"
frag_quote="\""
frag_colon=":"
frag_comma=","
key_id="\"id\""
key_name="\"name\""
key_arguments="\"arguments\""
sample_args="{\"x\":1}"
sample_nested="{\"a\":{\"b\":2}}"
EOF
fi

if [ -f "$build_dir/build.ninja" ]; then
    CC=${CC:-clang} meson setup --reconfigure "$build_dir" -Dfuzz=true -Dtests=false -Dexamples=false
else
    CC=${CC:-clang} meson setup "$build_dir" -Dfuzz=true -Dtests=false -Dexamples=false
fi

meson compile -C "$build_dir"

runs=${FUZZ_RUNS:-20000}
max_len=${FUZZ_MAX_LEN:-4096}
timeout=${FUZZ_TIMEOUT:-5}

"$build_dir/fuzz_sse_scanner" -dict="$dict_dir/sse.dict" -runs="$runs" -max_len="$max_len" -timeout="$timeout"
"$build_dir/fuzz_sse_fragmented" -dict="$dict_dir/sse.dict" -runs="$runs" -max_len="$max_len" -timeout="$timeout"
"$build_dir/fuzz_sse_config" -dict="$dict_dir/sse.dict" -runs="$runs" -max_len="$max_len" -timeout="$timeout"
"$build_dir/fuzz_sse_writer_roundtrip" -dict="$dict_dir/sse.dict" -runs="$runs" -max_len="$max_len" -timeout="$timeout"
"$build_dir/fuzz_json_spans" -dict="$dict_dir/json_spans.dict" -runs="$runs" -max_len="$max_len" -timeout="$timeout"
"$build_dir/fuzz_tool_accum" -dict="$dict_dir/tool_accum.dict" -runs="$runs" -max_len="$max_len" -timeout="$timeout"
