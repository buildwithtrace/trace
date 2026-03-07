# usearch - Header-only HNSW vector search library
# https://github.com/unum-cloud/usearch (Apache-2.0)

add_library( usearch INTERFACE )
target_include_directories( usearch SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Disable fp16 library dependency - we don't have fp16/fp16.h and don't need it.
# The library will use software emulation for fp16 conversions instead.
target_compile_definitions( usearch INTERFACE USEARCH_USE_FP16LIB=0 )
