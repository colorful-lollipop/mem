add_executable(memrpc_minirpc_demo minirpc_demo_main.cpp)
target_link_libraries(memrpc_minirpc_demo PRIVATE minirpc_demo)

# vpsdemo - VPS demo application
# Default to tests on; allow callers to override.
set(VPSDEMO_ENABLE_TESTS ON CACHE BOOL "Build vpsdemo tests")
# Tie vpsdemo fuzz to the top-level fuzz switch unless explicitly overridden.
set(VPSDEMO_ENABLE_FUZZ ${MEMRPC_ENABLE_FUZZ} CACHE BOOL "Build vpsdemo fuzz targets")
add_subdirectory(vpsdemo)
