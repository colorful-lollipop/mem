include_guard(GLOBAL)

set(MEMRPC_DEFAULT_ENABLE_STRICT_WARNINGS ON)
set(MEMRPC_DEFAULT_WARNINGS_AS_ERRORS OFF)
if (DEFINED ENV{CI} AND NOT "$ENV{CI}" STREQUAL "")
  set(MEMRPC_DEFAULT_WARNINGS_AS_ERRORS ON)
endif()

option(MEMRPC_ENABLE_STRICT_WARNINGS "Enable stricter warning flags for project code" ${MEMRPC_DEFAULT_ENABLE_STRICT_WARNINGS})
option(MEMRPC_WARNINGS_AS_ERRORS "Treat warnings as errors for project code" ${MEMRPC_DEFAULT_WARNINGS_AS_ERRORS})
option(MEMRPC_ENABLE_CLANG_TIDY "Run clang-tidy during C++ compilation" OFF)
option(MEMRPC_CLANG_TIDY_AS_ERRORS "Treat clang-tidy diagnostics as errors" ${MEMRPC_DEFAULT_WARNINGS_AS_ERRORS})
option(MEMRPC_CLANG_TIDY_MAINLINE_ONLY
  "Skip clang-tidy for test, mock, and testkit targets"
  ON)
option(MEMRPC_ENABLE_ASAN "Enable AddressSanitizer instrumentation" OFF)
option(MEMRPC_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer instrumentation" OFF)
option(MEMRPC_ENABLE_TSAN "Enable ThreadSanitizer instrumentation" OFF)

function(memrpc_disable_clang_tidy_for_target target)
  if (NOT TARGET "${target}")
    message(FATAL_ERROR "memrpc_disable_clang_tidy_for_target: unknown target '${target}'")
  endif()
  set_target_properties("${target}" PROPERTIES CXX_CLANG_TIDY "")
endfunction()

function(memrpc_configure_project_options)
  if (DEFINED MEMRPC_PROJECT_OPTIONS_CONFIGURED)
    return()
  endif()

  if (MEMRPC_ENABLE_TSAN AND (MEMRPC_ENABLE_ASAN OR MEMRPC_ENABLE_UBSAN))
    message(FATAL_ERROR
      "MEMRPC_ENABLE_TSAN cannot be combined with MEMRPC_ENABLE_ASAN or MEMRPC_ENABLE_UBSAN")
  endif()

  set(project_compile_options "")
  set(project_link_options "")
  set(sanitizer_flags "")

  if (MEMRPC_ENABLE_STRICT_WARNINGS)
    list(APPEND project_compile_options
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wundef
      -Wformat=2
      -Wnull-dereference
      -Wimplicit-fallthrough
    )
  endif()

  if (MEMRPC_WARNINGS_AS_ERRORS)
    list(APPEND project_compile_options -Werror)
  endif()

  if (MEMRPC_ENABLE_ASAN)
    list(APPEND sanitizer_flags -fsanitize=address)
  endif()

  if (MEMRPC_ENABLE_UBSAN)
    list(APPEND sanitizer_flags -fsanitize=undefined)
  endif()

  if (MEMRPC_ENABLE_TSAN)
    list(APPEND sanitizer_flags -fsanitize=thread)
  endif()

  if (sanitizer_flags)
    list(APPEND project_compile_options
      -g3
      -O1
      -fno-omit-frame-pointer
      -fno-sanitize-recover=all
    )
    list(APPEND project_compile_options ${sanitizer_flags})
    list(APPEND project_link_options
      -fno-sanitize-recover=all
    )
    list(APPEND project_link_options ${sanitizer_flags})
  endif()

  foreach(option IN LISTS project_compile_options)
    add_compile_options("${option}")
  endforeach()

  foreach(option IN LISTS project_link_options)
    add_link_options("${option}")
  endforeach()

  if (MEMRPC_ENABLE_CLANG_TIDY)
    find_program(MEMRPC_CLANG_TIDY_EXECUTABLE
      NAMES
        clang-tidy
        clang-tidy-19
        clang-tidy-18
        clang-tidy-17
        clang-tidy-16
        clang-tidy-15
        clang-tidy-14
    )
    if (NOT MEMRPC_CLANG_TIDY_EXECUTABLE)
      message(FATAL_ERROR
        "MEMRPC_ENABLE_CLANG_TIDY is ON, but no clang-tidy executable was found in PATH")
    endif()

    set(clang_tidy_command "${MEMRPC_CLANG_TIDY_EXECUTABLE}")
    if (MEMRPC_CLANG_TIDY_AS_ERRORS)
      list(APPEND clang_tidy_command "-warnings-as-errors=*")
    endif()
    set(CMAKE_CXX_CLANG_TIDY "${clang_tidy_command}" CACHE STRING
        "clang-tidy command line for C++ targets" FORCE)
    message(STATUS "clang-tidy enabled: ${MEMRPC_CLANG_TIDY_EXECUTABLE}")
  endif()

  set(MEMRPC_PROJECT_OPTIONS_CONFIGURED TRUE CACHE INTERNAL
      "Whether global memrpc project options were already configured")
endfunction()
