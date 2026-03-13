include_guard(GLOBAL)

option(MEMRPC_ENABLE_STRICT_WARNINGS "Enable stricter warning flags for project code" OFF)
option(MEMRPC_WARNINGS_AS_ERRORS "Treat warnings as errors for project code" OFF)
option(MEMRPC_ENABLE_ASAN "Enable AddressSanitizer instrumentation" OFF)
option(MEMRPC_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer instrumentation" OFF)
option(MEMRPC_ENABLE_TSAN "Enable ThreadSanitizer instrumentation" OFF)

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

  set(MEMRPC_PROJECT_OPTIONS_CONFIGURED TRUE CACHE INTERNAL
      "Whether global memrpc project options were already configured")
endfunction()
