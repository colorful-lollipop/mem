add_executable(memrpc_minirpc_demo minirpc_demo_main.cpp)
target_link_libraries(memrpc_minirpc_demo PRIVATE minirpc_demo)

# vpsdemo - VPS demo application
set(VPSDEMO_ENABLE_TESTS ON CACHE BOOL "Build vpsdemo tests" FORCE)
set(VPSDEMO_ENABLE_FUZZ OFF CACHE BOOL "Build vpsdemo fuzz targets" FORCE)
add_subdirectory(vpsdemo)
