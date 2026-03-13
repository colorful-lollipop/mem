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
  --strict            Enable stricter warning flags for project code
  --werror            Treat warnings as errors for project code
  --asan              Enable AddressSanitizer + UndefinedBehaviorSanitizer
                      and leak detection at test time
  --ubsan             Enable UndefinedBehaviorSanitizer only
  --tsan              Enable ThreadSanitizer
  --fuzz              Enable memrpc + virus_executor_service fuzz targets
  --clean             Remove the build directory before configuring
  --configure-only    Configure only
  --build-only        Configure and build, but do not run tests
  --test-regex REGEX  Only run tests matching the regex
  --label LABEL       Only run tests with the CTest label
  --repeat MODE       Pass through to ctest --repeat (e.g. until-fail:100)
  --timeout SEC       Pass through to ctest --timeout
  -h, --help          Show this help message

Examples:
  tools/build_and_test.sh
  tools/build_and_test.sh --asan --clean
  tools/build_and_test.sh --tsan --test-regex memrpc
  tools/build_and_test.sh --clean --build-dir build_full --fuzz
  tools/build_and_test.sh --test-regex virus_executor_service
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
build_dir_explicit=0
jobs="${JOBS:-$(detect_jobs)}"
enable_fuzz="OFF"
enable_strict="OFF"
warnings_as_errors="OFF"
enable_asan="OFF"
enable_ubsan="OFF"
enable_tsan="OFF"
clean_build=0
run_configure=1
run_build=1
run_tests=1
test_regex=""
test_label=""
test_repeat=""
test_timeout=""
extra_cmake_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || die "--build-dir requires a value"
            build_dir="$2"
            build_dir_explicit=1
            shift 2
            ;;
        --jobs)
            [[ $# -ge 2 ]] || die "--jobs requires a value"
            jobs="$2"
            shift 2
            ;;
        --strict)
            enable_strict="ON"
            shift
            ;;
        --werror)
            warnings_as_errors="ON"
            shift
            ;;
        --asan)
            enable_asan="ON"
            enable_ubsan="ON"
            shift
            ;;
        --ubsan)
            enable_ubsan="ON"
            shift
            ;;
        --tsan)
            enable_tsan="ON"
            shift
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
        --repeat)
            [[ $# -ge 2 ]] || die "--repeat requires a value"
            test_repeat="$2"
            shift 2
            ;;
        --timeout)
            [[ $# -ge 2 ]] || die "--timeout requires a value"
            test_timeout="$2"
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

if [[ "${enable_tsan}" == "ON" && ( "${enable_asan}" == "ON" || "${enable_ubsan}" == "ON" ) ]]; then
    die "--tsan cannot be combined with --asan or --ubsan"
fi

if [[ "${build_dir_explicit}" -eq 0 ]]; then
    if [[ "${enable_tsan}" == "ON" ]]; then
        build_dir="${repo_root}/build_tsan"
    elif [[ "${enable_asan}" == "ON" || "${enable_ubsan}" == "ON" ]]; then
        build_dir="${repo_root}/build_asan"
    else
        build_dir="${repo_root}/build_ninja"
    fi
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
    -DMEMRPC_ENABLE_STRICT_WARNINGS="${enable_strict}"
    -DMEMRPC_WARNINGS_AS_ERRORS="${warnings_as_errors}"
    -DMEMRPC_ENABLE_ASAN="${enable_asan}"
    -DMEMRPC_ENABLE_UBSAN="${enable_ubsan}"
    -DMEMRPC_ENABLE_TSAN="${enable_tsan}"
    -DVIRUS_EXECUTOR_SERVICE_ENABLE_TESTS=ON
    -DVIRUS_EXECUTOR_SERVICE_ENABLE_FUZZ="${enable_fuzz}"
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

    if [[ -n "${test_repeat}" ]]; then
        ctest_args+=(--repeat "${test_repeat}")
    fi

    if [[ -n "${test_timeout}" ]]; then
        ctest_args+=(--timeout "${test_timeout}")
    fi

    test_env=()
    if [[ "${enable_asan}" == "ON" ]]; then
        test_env+=(
            "ASAN_OPTIONS=detect_leaks=1:check_initialization_order=1:strict_string_checks=1:detect_stack_use_after_return=1"
            "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
        )
    elif [[ "${enable_ubsan}" == "ON" ]]; then
        test_env+=("UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
    fi

    if [[ "${enable_tsan}" == "ON" ]]; then
        test_env+=("TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:history_size=7")
    fi

    log_phase "Running tests from ${build_dir}"
    if [[ ${#test_env[@]} -gt 0 ]]; then
        env "${test_env[@]}" ctest "${ctest_args[@]}"
    else
        ctest "${ctest_args[@]}"
    fi
fi
