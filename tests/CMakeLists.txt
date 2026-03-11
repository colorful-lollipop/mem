add_subdirectory(memrpc)
add_subdirectory(apps/minirpc)

if (MEMRPC_ENABLE_FUZZ)
  add_subdirectory(fuzz)
endif()
