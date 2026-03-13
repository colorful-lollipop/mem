#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/ci_sweep.sh [options]

Run a practical local verification matrix across multiple build directories.

Default sweep:
  1. strict warnings + full test suite
  2. ASan/UBSan + full test suite
  3. TSan + high-risk concurrency subset
  4. repeat the shutdown-race focused tests

Options:
  --jobs N         Parallel build/test jobs (default: auto-detect)
  --no-tsan        Skip the TSan pass
  --no-repeat      Skip the repeated race-regression pass
  --keep-builds    Reuse existing build dirs instead of cleaning them first
  -h, --help       Show this help
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
run_tsan=1
run_repeat=1
clean_flag="--clean"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs)
            [[ $# -ge 2 ]] || { echo "error: --jobs requires a value" >&2; exit 1; }
            jobs="$2"
            shift 2
            ;;
        --no-tsan)
            run_tsan=0
            shift
            ;;
        --no-repeat)
            run_repeat=0
            shift
            ;;
        --keep-builds)
            clean_flag=""
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

run_build_and_test() {
    local mode="$1"
    shift
    echo
    echo "==> ci_sweep: ${mode}"
    "${repo_root}/tools/build_and_test.sh" ${clean_flag} --jobs "${jobs}" "$@"
}

run_build_and_test "strict-full" \
    --build-dir "${repo_root}/build_ninja" \
    --strict

run_build_and_test "asan-full" \
    --build-dir "${repo_root}/build_asan" \
    --asan \
    --strict \
    --timeout 90

if [[ "${run_tsan}" -eq 1 ]]; then
    run_build_and_test "tsan-concurrency" \
        --build-dir "${repo_root}/build_tsan" \
        --tsan \
        --strict \
        --timeout 90 \
        --test-regex 'memrpc_(engine_death_handler|rpc_client_.*|typed_future|session|dt_.*)|virus_executor_service_(session_service|heartbeat|testkit_.*|policy|crash_recovery|supervisor_integration|stress_test|dt_crash_recovery_test)'
fi

if [[ "${run_repeat}" -eq 1 ]]; then
    run_build_and_test "repeat-race-regressions" \
        --build-dir "${repo_root}/build_ninja" \
        --strict \
        --test-regex 'memrpc_engine_death_handler_test|memrpc_rpc_client_shutdown_race_test' \
        --repeat until-fail:100 \
        --timeout 30
fi
