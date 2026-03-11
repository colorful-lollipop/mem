add_subdirectory(memrpc)
add_subdirectory(apps/minirpc)
add_subdirectory(apps/vps)

if (MEMRPC_ENABLE_FUZZ)
  add_subdirectory(fuzz)
endif()
