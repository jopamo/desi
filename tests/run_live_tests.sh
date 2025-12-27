#!/bin/sh
set -eu

if ! command -v meson >/dev/null 2>&1; then
    echo "meson not found in PATH" >&2
    exit 1
fi

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$root_dir/build-live"}

if [ -z "${LLM_BASE_URL:-}" ]; then
    echo "LLM_BASE_URL is required for live tests" >&2
    exit 1
fi
if [ -z "${LLM_LIVE_TESTS:-}" ]; then
    export LLM_LIVE_TESTS=1
fi

echo "LLM_BASE_URL=$LLM_BASE_URL"
if [ -n "${LLM_MODEL:-}" ]; then
    echo "LLM_MODEL=$LLM_MODEL"
else
    echo "LLM_MODEL is not set (tests will use their defaults)"
fi

if [ -f "$build_dir/build.ninja" ]; then
    if [ -n "${CC:-}" ]; then
        CC="$CC" meson setup --reconfigure "$build_dir" -Dtests=true -Dexamples=false
    else
        meson setup --reconfigure "$build_dir" -Dtests=true -Dexamples=false
    fi
else
    if [ -n "${CC:-}" ]; then
        CC="$CC" meson setup "$build_dir" -Dtests=true -Dexamples=false
    else
        meson setup "$build_dir" -Dtests=true -Dexamples=false
    fi
fi

meson compile -C "$build_dir"
meson test -C "$build_dir" --suite live --print-errorlogs
