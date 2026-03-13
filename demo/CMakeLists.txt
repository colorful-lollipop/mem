# vpsdemo is the repository mainline. memrpc is built as its dependency module.
# Default to tests on; allow callers to override.
set(VPSDEMO_ENABLE_TESTS ON CACHE BOOL "Build vpsdemo tests")
# Tie vpsdemo fuzz to the top-level fuzz switch unless explicitly overridden.
set(VPSDEMO_ENABLE_FUZZ ${MEMRPC_ENABLE_FUZZ} CACHE BOOL "Build vpsdemo fuzz targets")
add_subdirectory(vpsdemo)
