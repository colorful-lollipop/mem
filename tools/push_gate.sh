#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/push_gate.sh [options]

Run the repository push gate.

Default gate:
  1. clang-tidy + warning-as-error build
  2. tools/ci_sweep.sh --no-tidy
  3. repeat shutdown/death-handler regressions 300 times

Deep gate:
  1. clang-tidy + warning-as-error build
  2. tools/ci_sweep.sh --no-tidy
  3. repeat shutdown/death-handler regressions 1000 times
  4. repeat a wider concurrency subset 200 times

Options:
  --jobs N          Parallel build/test jobs (default: auto-detect)
  --deep            Run the deeper pre-push gate
  --keep-builds     Reuse existing build dirs instead of cleaning first
  --skip-tidy       Skip the clang-tidy phase
  --skip-tsan       Skip the TSan phase inside ci_sweep
  --help, -h        Show this help
EOF
}

detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
        return
    fi
    echo 8
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

jobs="${JOBS:-$(detect_jobs)}"
deep=0
keep_builds=0
skip_tidy=0
skip_tsan=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs)
            [[ $# -ge 2 ]] || { echo "error: --jobs requires a value" >&2; exit 1; }
            jobs="$2"
            shift 2
            ;;
        --deep)
            deep=1
            shift
            ;;
        --keep-builds)
            keep_builds=1
            shift
            ;;
        --skip-tidy)
            skip_tidy=1
            shift
            ;;
        --skip-tsan)
            skip_tsan=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            exit 1
            ;;
    esac
done

ci_args=(--jobs "${jobs}")
if [[ "${keep_builds}" -eq 1 ]]; then
    ci_args+=(--keep-builds)
fi
ci_args+=(--no-tidy)
if [[ "${skip_tsan}" -eq 1 ]]; then
    ci_args+=(--no-tsan)
fi

build_clean_flag="--clean"
if [[ "${keep_builds}" -eq 1 ]]; then
    build_clean_flag=""
fi

detect_clang_tidy() {
    command -v clang-tidy >/dev/null 2>&1 && return 0
    command -v clang-tidy-19 >/dev/null 2>&1 && return 0
    command -v clang-tidy-18 >/dev/null 2>&1 && return 0
    command -v clang-tidy-17 >/dev/null 2>&1 && return 0
    command -v clang-tidy-16 >/dev/null 2>&1 && return 0
    command -v clang-tidy-15 >/dev/null 2>&1 && return 0
    command -v clang-tidy-14 >/dev/null 2>&1 && return 0
    return 1
}

if [[ "${skip_tidy}" -eq 0 ]]; then
    if detect_clang_tidy; then
        echo
        echo "==> push_gate: clang-tidy"
        "${repo_root}/tools/build_and_test.sh" ${build_clean_flag} \
            --build-dir "${repo_root}/build_tidy" \
            --strict \
            --werror \
            --clang-tidy \
            --jobs "${jobs}" \
            --build-only
    else
        echo
        echo "==> push_gate: clang-tidy (skipped: clang-tidy not found in PATH)"
    fi
fi

echo
echo "==> push_gate: ci_sweep"
"${repo_root}/tools/ci_sweep.sh" "${ci_args[@]}"

repeat_count=300
wide_repeat_count=0
if [[ "${deep}" -eq 1 ]]; then
    repeat_count=1000
    wide_repeat_count=200
fi

echo
echo "==> push_gate: focused repeat regressions (${repeat_count}x)"
"${repo_root}/tools/build_and_test.sh" \
    --build-dir "${repo_root}/build_ninja" \
    --strict \
    --werror \
    --jobs "${jobs}" \
    --test-regex 'memrpc_engine_death_handler_test|memrpc_rpc_client_shutdown_race_test' \
    --repeat "until-fail:${repeat_count}" \
    --timeout 30

if [[ "${wide_repeat_count}" -gt 0 ]]; then
    echo
    echo "==> push_gate: wide concurrency repeat regressions (${wide_repeat_count}x)"
    "${repo_root}/tools/build_and_test.sh" \
        --build-dir "${repo_root}/build_ninja" \
        --strict \
        --werror \
        --jobs "${jobs}" \
        --test-regex 'memrpc_(rpc_client_.*|typed_future|engine_death_handler_test)|virus_executor_service_(testkit_client_test|testkit_dfx_test|testkit_async_pipeline_test|heartbeat_test|health_subscription_test|recovery_reason_test|session_service_test|policy_test)' \
        --repeat "until-fail:${wide_repeat_count}" \
        --timeout 45
fi

echo
echo "push_gate: PASS"
