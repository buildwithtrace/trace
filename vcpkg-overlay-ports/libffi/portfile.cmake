# Industry standard: libffi autotools build on Windows has issues with debug builds
# The debug and release builds overwrite each other due to DESTDIR/prefix handling
# Solution: Build release only and copy to debug location for python3 compatibility
if(VCPKG_TARGET_IS_WINDOWS)
    set(VCPKG_BUILD_TYPE release)
endif()

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/libffi/libffi/releases/download/v${VERSION}/libffi-${VERSION}.tar.gz"
    FILENAME "libffi-${VERSION}.tar.gz"
    SHA512 3da9e21fdb920e7962ceb01ee671ef36196df4d5dad62e0cdd8e87cc60e350f241c204350560ae26ea04cc898161b5585c8a5a5125bdbcc84508efbb7ea61eb8
)
vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        dll-bindir.diff
)

vcpkg_list(SET options)
if(VCPKG_TARGET_IS_WINDOWS)
    set(linkage_flag "-DFFI_STATIC_BUILD")
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
        set(linkage_flag "-DFFI_BUILDING_DLL")
    endif()
    vcpkg_list(APPEND options "CFLAGS=\${CFLAGS} ${linkage_flag}")
endif()

vcpkg_cmake_get_vars(cmake_vars_file ADDITIONAL_LANGUAGES ASM)
include("${cmake_vars_file}")
if(VCPKG_DETECTED_CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    vcpkg_add_to_path("${SOURCE_PATH}")
    vcpkg_list(APPEND options "CCAS=msvcc.sh")
    set(ccas_options "")
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
        string(APPEND ccas_options " -m32")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        string(APPEND ccas_options " -m64")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm")
        string(APPEND ccas_options " -marm")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        string(APPEND ccas_options " -marm64")
    endif()
    if(ccas_options)
        vcpkg_list(APPEND options "CCASFLAGS=\${CCASFLAGS}${ccas_options}")
    endif()
endif()

vcpkg_make_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    LANGUAGES C CXX ASM
    OPTIONS
        --enable-portable-binary
        --disable-docs
        --disable-multi-os-directory
        ${options}
)

vcpkg_make_install()
vcpkg_copy_pdbs()

# Fix installation paths - autotools installs to usr/local/ instead of root
# Move all files from usr/local/ to the package root and fix pkgconfig paths
if(EXISTS "${CURRENT_PACKAGES_DIR}/usr/local")
    file(GLOB_RECURSE USR_LOCAL_FILES "${CURRENT_PACKAGES_DIR}/usr/local/*")
    foreach(FILE_PATH ${USR_LOCAL_FILES})
        string(REPLACE "${CURRENT_PACKAGES_DIR}/usr/local/" "${CURRENT_PACKAGES_DIR}/" NEW_PATH "${FILE_PATH}")
        get_filename_component(NEW_DIR "${NEW_PATH}" DIRECTORY)
        file(MAKE_DIRECTORY "${NEW_DIR}")
        file(RENAME "${FILE_PATH}" "${NEW_PATH}")
    endforeach()
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/usr")
    
    # Fix prefix path in pkgconfig file
    if(EXISTS "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libffi.pc")
        file(READ "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libffi.pc" PC_CONTENT)
        string(REPLACE "prefix=/usr/local" "prefix=\${pcfiledir}/../.." PC_CONTENT "${PC_CONTENT}")
        file(WRITE "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libffi.pc" "${PC_CONTENT}")
    endif()
endif()
if(EXISTS "${CURRENT_PACKAGES_DIR}/debug/usr/local")
    file(GLOB_RECURSE USR_LOCAL_FILES "${CURRENT_PACKAGES_DIR}/debug/usr/local/*")
    foreach(FILE_PATH ${USR_LOCAL_FILES})
        string(REPLACE "${CURRENT_PACKAGES_DIR}/debug/usr/local/" "${CURRENT_PACKAGES_DIR}/debug/" NEW_PATH "${FILE_PATH}")
        get_filename_component(NEW_DIR "${NEW_PATH}" DIRECTORY)
        file(MAKE_DIRECTORY "${NEW_DIR}")
        file(RENAME "${FILE_PATH}" "${NEW_PATH}")
    endforeach()
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/usr")
    
    # Fix prefix path in debug pkgconfig file
    if(EXISTS "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libffi.pc")
        file(READ "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libffi.pc" PC_CONTENT)
        string(REPLACE "prefix=/usr/local" "prefix=\${pcfiledir}/../.." PC_CONTENT "${PC_CONTENT}")
        file(WRITE "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libffi.pc" "${PC_CONTENT}")
    endif()
endif()

vcpkg_fixup_pkgconfig()

# On Windows, copy release lib to debug location for python3 compatibility
# python3 portfile looks for ffi.lib in both lib/ and debug/lib/
if(VCPKG_TARGET_IS_WINDOWS)
    file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/debug/lib")
    if(EXISTS "${CURRENT_PACKAGES_DIR}/lib/ffi.lib")
        file(COPY "${CURRENT_PACKAGES_DIR}/lib/ffi.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
    endif()
    # Copy pkgconfig for debug as well
    if(EXISTS "${CURRENT_PACKAGES_DIR}/lib/pkgconfig")
        file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig")
        file(GLOB PC_FILES "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/*.pc")
        file(COPY ${PC_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig")
    endif()
endif()

if (VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/ffi.h" "defined(FFI_STATIC_BUILD)" "1")
endif()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/unofficial-libffi-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/unofficial-libffi")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/libffiConfig.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/man3"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
