#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/build_and_test.sh [options] [-- <extra-cmake-args>...]

Configure, build, and test the whole repository with CMake + Clang + Ninja.

Notes:
  - Default build directory is isolated as ./build_ninja to avoid generator clashes.
  - In sandboxed AI sessions, shared-memory and registry tests may require elevated
    permissions when running CTest.

Options:
  --build-dir DIR     Build directory (default: ./build_ninja)
  --jobs N            Parallel build/test jobs (default: auto-detect)
  --fuzz              Enable memrpc + vpsdemo fuzz targets
  --clean             Remove the build directory before configuring
  --configure-only    Configure only
  --build-only        Configure and build, but do not run tests
  --test-regex REGEX  Only run tests matching the regex
  --label LABEL       Only run tests with the CTest label
  -h, --help          Show this help message

Examples:
  tools/build_and_test.sh
  tools/build_and_test.sh --clean --build-dir build_full --fuzz
  tools/build_and_test.sh --test-regex vpsdemo
  tools/build_and_test.sh --label fuzz --fuzz
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
        return
    fi

    if command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN
        return
    fi

    echo 8
}

log_phase() {
    printf '\n==> %s\n' "$1"
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

build_dir="${repo_root}/build_ninja"
jobs="${JOBS:-$(detect_jobs)}"
enable_fuzz="OFF"
clean_build=0
run_configure=1
run_build=1
run_tests=1
test_regex=""
test_label=""
extra_cmake_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || die "--build-dir requires a value"
            build_dir="$2"
            shift 2
            ;;
        --jobs)
            [[ $# -ge 2 ]] || die "--jobs requires a value"
            jobs="$2"
            shift 2
            ;;
        --fuzz)
            enable_fuzz="ON"
            shift
            ;;
        --clean)
            clean_build=1
            shift
            ;;
        --configure-only)
            run_build=0
            run_tests=0
            shift
            ;;
        --build-only)
            run_tests=0
            shift
            ;;
        --test-regex)
            [[ $# -ge 2 ]] || die "--test-regex requires a value"
            test_regex="$2"
            shift 2
            ;;
        --label)
            [[ $# -ge 2 ]] || die "--label requires a value"
            test_label="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            extra_cmake_args+=("$@")
            break
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

if [[ -n "${test_regex}" && -n "${test_label}" ]]; then
    die "--test-regex and --label cannot be used together"
fi

require_cmd cmake
require_cmd ctest
require_cmd ninja
require_cmd clang
require_cmd clang++

cache_file="${build_dir}/CMakeCache.txt"
if [[ -f "${cache_file}" && "${clean_build}" -eq 0 ]]; then
    current_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "${cache_file}")"
    if [[ -n "${current_generator}" && "${current_generator}" != "Ninja" ]]; then
        die "build directory ${build_dir} was configured with '${current_generator}'; rerun with --clean or choose --build-dir"
    fi
fi

if [[ "${clean_build}" -eq 1 ]]; then
    log_phase "Cleaning ${build_dir}"
    rm -rf "${build_dir}"
fi

mkdir -p "${build_dir}"

cmake_args=(
    -S "${repo_root}"
    -B "${build_dir}"
    -G Ninja
    -DCMAKE_C_COMPILER=clang
    -DCMAKE_CXX_COMPILER=clang++
    -DVPSDEMO_ENABLE_TESTS=ON
    -DVPSDEMO_ENABLE_FUZZ="${enable_fuzz}"
    -DMEMRPC_ENABLE_DT_TESTS=ON
    -DMEMRPC_ENABLE_STRESS_TESTS=ON
    -DMEMRPC_ENABLE_FUZZ="${enable_fuzz}"
)
cmake_args+=("${extra_cmake_args[@]}")

if [[ "${run_configure}" -eq 1 ]]; then
    log_phase "Configuring ${build_dir}"
    cmake "${cmake_args[@]}"
fi

if [[ "${run_build}" -eq 1 ]]; then
    log_phase "Building ${build_dir}"
    cmake --build "${build_dir}" --parallel "${jobs}"
fi

if [[ "${run_tests}" -eq 1 ]]; then
    ctest_args=(
        --test-dir "${build_dir}"
        --output-on-failure
        --parallel "${jobs}"
    )

    if [[ -n "${test_regex}" ]]; then
        ctest_args+=(-R "${test_regex}")
    fi

    if [[ -n "${test_label}" ]]; then
        ctest_args+=(-L "${test_label}")
    fi

    log_phase "Running tests from ${build_dir}"
    ctest "${ctest_args[@]}"
fi
