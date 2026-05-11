# ONNX Runtime - Pre-built C++ inference library
# https://github.com/microsoft/onnxruntime (MIT)
#
# Pre-built binaries must be downloaded for the target platform:
#   macOS arm64: https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-osx-arm64-1.20.1.tgz
#   macOS x86:   https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-osx-x86_64-1.20.1.tgz
#   Linux x64:   https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-linux-x64-1.20.1.tgz
#   Windows x64: https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip

set( ORT_ROOT ${CMAKE_CURRENT_SOURCE_DIR} )

if( EXISTS "${ORT_ROOT}/lib" AND EXISTS "${ORT_ROOT}/include" )
    add_library( onnxruntime SHARED IMPORTED GLOBAL )

    if( APPLE )
        file( GLOB ORT_DYLIBS "${ORT_ROOT}/lib/libonnxruntime*.dylib" )
        list( GET ORT_DYLIBS 0 ORT_LIB )
        set_target_properties( onnxruntime PROPERTIES
            IMPORTED_LOCATION "${ORT_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${ORT_ROOT}/include"
        )
    elseif( WIN32 )
        set_target_properties( onnxruntime PROPERTIES
            IMPORTED_IMPLIB "${ORT_ROOT}/lib/onnxruntime.lib"
            IMPORTED_LOCATION "${ORT_ROOT}/lib/onnxruntime.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${ORT_ROOT}/include"
        )
    else()
        file( GLOB ORT_SOS "${ORT_ROOT}/lib/libonnxruntime.so*" )
        list( GET ORT_SOS 0 ORT_LIB )
        set_target_properties( onnxruntime PROPERTIES
            IMPORTED_LOCATION "${ORT_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${ORT_ROOT}/include"
        )
    endif()

    message( STATUS "Found ONNX Runtime at ${ORT_ROOT}" )
else()
    message( WARNING "ONNX Runtime pre-built binaries not found at ${ORT_ROOT}. "
                     "Vector search will fall back to Python subprocess. "
                     "Download from: https://github.com/microsoft/onnxruntime/releases/tag/v1.20.1" )

    # Create a dummy interface target so CMake doesn't fail
    add_library( onnxruntime INTERFACE )
    target_compile_definitions( onnxruntime INTERFACE TRACE_NO_ONNXRUNTIME=1 )
endif()
