vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wxWidgets/phoenix
    REF a14508320d752b2c95ec1808c4c7bf128fff06ac
    SHA512 1989510a40128ec99c89ba84f7880e9e4d7dba590b28978f2ffa3e6bcb0033e256cef967d9921771e8c931276a1ade6d228f163181b8b9b0196fc344c43ab060
    PATCHES
        fixup-for-latest-wx-4e4159.patch
)

# We need a copy of the wxWidgets source code to build doxygen xml, the doxygen xml is used to generate the C++ code
# This should match the vcpkg port of wxWidgets
# THIS DOES NOT GET COMPILED
vcpkg_from_github(    
    OUT_SOURCE_PATH WX_SOURCE_PATH
    REPO wxWidgets/wxWidgets
    REF 8245e1073ae66fd246d2ec180160d2e20acf3644
    SHA512 6bcd30d60de73d3189e8a60b1e21fae352e33df9ffa6799f59ab23134fd5b24b503cba87d0c726bb1111edc50be3bb1cd8cdc32914e20e4769ff0dec01589391
    HEAD_REF master
)

## Create __version__.py
set(VERSION_MAJOR 4)
set(VERSION_MINOR 2)
set(VERSION_RELEASE 2)
set(VERSION_STRING "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_RELEASE}")
set(VERSION_BUILD_TYPE "")
configure_file( "${CMAKE_CURRENT_LIST_DIR}/cmake/__version__.py.in" "${SOURCE_PATH}/wx/__version__.py" @ONLY )

find_program(GIT NAMES git git.cmd)

file(COPY ${WX_SOURCE_PATH}/	DESTINATION ${SOURCE_PATH}/ext/wxWidgets/)
file(COPY ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt DESTINATION ${SOURCE_PATH})
file(COPY ${CMAKE_CURRENT_LIST_DIR}/cmake/ DESTINATION ${SOURCE_PATH}/cmake/)

# Fix wxAuiDefaultToolBarArt doxygen interface: actual headers use wxReadOnlyDC& for
# GetLabelSize/GetToolSize but the interface file still uses wxDC&, causing override mismatch
set(_auibar_iface "${SOURCE_PATH}/ext/wxWidgets/interface/wx/aui/auibar.h")
file(READ "${_auibar_iface}" _content)
string(REPLACE "virtual wxSize GetLabelSize(
                wxDC& dc," "virtual wxSize GetLabelSize(
                wxReadOnlyDC& dc," _content "${_content}")
string(REPLACE "virtual wxSize GetToolSize(
                wxDC& dc," "virtual wxSize GetToolSize(
                wxReadOnlyDC& dc," _content "${_content}")
file(WRITE "${_auibar_iface}" "${_content}")

###
# Overcomplicated Python Mess
# lets download a matching python version for our environment for only building wxpython
###

if(DEFINED ENV{PROCESSOR_ARCHITEW6432})
    set(build_arch $ENV{PROCESSOR_ARCHITEW6432})
else()
    set(build_arch $ENV{PROCESSOR_ARCHITECTURE})
endif()

set(PYTHON_TOOL_VERSION "3.11.8")

if(build_arch MATCHES "^(ARM|arm)64$")
    set(tool_subdirectory "python-${PYTHON_TOOL_VERSION}-arm64")
    set(download_urls "https://www.python.org/ftp/python/${PYTHON_TOOL_VERSION}/python-${PYTHON_TOOL_VERSION}-embed-arm64.zip")
    set(download_filename "python-${PYTHON_TOOL_VERSION}-embed-arm64.zip")
    set(download_sha512 42b820e34c4a77fe928e0af395292d804dcbf7e1132cf353ce6ce23435a687ec580f03ccbf3cd94d98c9dc5ac951f8ca64dbd65cded7ef1d675a39d63f8ace8d)
elseif(build_arch MATCHES "(amd|AMD)64")
    set(tool_subdirectory "python-${PYTHON_TOOL_VERSION}-x64")
    set(download_urls "https://www.python.org/ftp/python/${PYTHON_TOOL_VERSION}/python-${PYTHON_TOOL_VERSION}-embed-amd64.zip")
    set(download_filename "python-${PYTHON_TOOL_VERSION}-embed-amd64.zip")
    set(download_sha512 da5f01e94d3505eebdfd4d2e70d9cf494925199024479cc29ef144567906b2e8ad55a855b199a755318f5fb9a260f21b987a5fc85f31acf631af4b677921251d)
else()
    set(tool_subdirectory "python-${PYTHON_TOOL_VERSION}-x86")
    set(download_urls "https://www.python.org/ftp/python/${PYTHON_TOOL_VERSION}/python-${PYTHON_TOOL_VERSION}-embed-win32.zip")
    set(download_filename "python-${PYTHON_TOOL_VERSION}-embed-win32.zip")
    set(download_sha512 c88ef02f0860000dbc59361cfe051e3e8dc7d208ed39bb5bc20a3e8b8711b578926e281a11941787ea61b2ef05b945ab3133322dcb85b916f79ac4ada57f6309)
endif()

set(paths_to_search "${DOWNLOADS}/tools/python/${tool_subdirectory}")

find_program(PYTHON3_TOOL_EXECUTABLE
    NAMES python
    PATHS ${paths_to_search}
    NO_DEFAULT_PATH)

if(${PYTHON3_TOOL_EXECUTABLE} STREQUAL "PYTHON3_TOOL_EXECUTABLE-NOTFOUND" )
    vcpkg_list(SET post_install_command
        "${CMAKE_COMMAND}" "-DPYTHON_DIR=${paths_to_search}" "-DPYTHON_VERSION=${PYTHON_TOOL_VERSION}" -P "${VCPKG_ROOT_DIR}/scripts/cmake/z_vcpkg_make_python_less_embedded.cmake"
    )
    vcpkg_download_distfile(archive_path
        URLS ${download_urls}
        SHA512 "${download_sha512}"
        FILENAME "${download_filename}"
    )

    set(full_subdirectory "${DOWNLOADS}/tools/python/${tool_subdirectory}")
    vcpkg_extract_archive(ARCHIVE "${archive_path}" DESTINATION "${full_subdirectory}")
    
    vcpkg_execute_required_process(
        ALLOW_IN_DOWNLOAD_MODE
        COMMAND ${post_install_command}
        WORKING_DIRECTORY "${full_subdirectory}"
        LOGNAME "python3-tool-post-install"
    )
            
    find_program(PYTHON3_TOOL_EXECUTABLE
        NAMES python
        PATHS ${paths_to_search}
        NO_DEFAULT_PATH)
        
endif()
    
x_vcpkg_get_python_packages(PYTHON_VERSION 3 PYTHON_EXECUTABLE "${PYTHON3_TOOL_EXECUTABLE}" OUT_PYTHON_VAR "PYTHON3" REQUIREMENTS_FILE ${SOURCE_PATH}/requirements.txt )
###
# END Overcomplicated Python Mess
###

# We need bash.exe for the wxPython preprocessor
if(CMAKE_HOST_WIN32)
    vcpkg_acquire_msys(MSYS_ROOT PACKAGES bash)
    vcpkg_add_to_path(PREPEND "${MSYS_ROOT}/usr/bin")

    # lazy hack to fix include path issue with the pip install of sip
    vcpkg_host_path_list(PREPEND ENV{INCLUDE} "${_VCPKG_INSTALLED_DIR}/${TARGET_TRIPLET}/include/python3.9/")
    vcpkg_host_path_list(PREPEND ENV{INCLUDE} "${_VCPKG_INSTALLED_DIR}/${TARGET_TRIPLET}/include/python3.11/")
    vcpkg_host_path_list(PREPEND ENV{LIB} "${_VCPKG_INSTALLED_DIR}/${TARGET_TRIPLET}/lib/")
endif()


vcpkg_execute_build_process(
    COMMAND ${PYTHON3} ./build.py dox
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME "prepare-dox-${RELEASE_TRIPLET}"
)

vcpkg_execute_build_process(
    COMMAND ${PYTHON3} ./build.py etg --nodoc sip
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME "prepare-etg-${RELEASE_TRIPLET}"
)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
)

vcpkg_cmake_install()

if(CMAKE_HOST_WIN32)
    unset(Python3_EXECUTABLE CACHE)
    unset(PYTHON3)
    unset(ENV{VIRTUAL_ENV})
    unset(ENV{PYTHONHOME})
    unset(ENV{PYTHONPATH})
    
    # this is a hack to make things work (i.e. just running from build dir and packaging)
	set(Python3_FIND_REGISTRY "NEVER")
    set(PYTHON3_ROOT_DIR "${_VCPKG_INSTALLED_DIR}/${HOST_TRIPLET}/tools/python3")
    vcpkg_add_to_path(PREPEND "${PYTHON3_ROOT_DIR}")

    set(ENV{PYTHONHOME} "${PYTHON3_ROOT_DIR}")
    set(ENV{PYTHONPATH} "${PYTHON3_ROOT_DIR}/DLLs;${PYTHON3_ROOT_DIR}/Lib")
    
    macro(z_vcpkg_warn_ambiguous_system_variables)
    # kill this dumb macro that whines on the stock cmake find module
    endmacro()
    find_package( Python3 COMPONENTS Interpreter REQUIRED )

    vcpkg_execute_build_process(
        COMMAND ${Python3_EXECUTABLE} -m ensurepip
        WORKING_DIRECTORY "${SOURCE_PATH}"
        LOGNAME "prepare-ensurepip-${RELEASE_TRIPLET}"
    )

    vcpkg_execute_build_process(
        COMMAND ${Python3_EXECUTABLE} -m pip install -t ${CURRENT_PACKAGES_DIR}/tools/python3/Lib/site-packages/ -r requirements/install.txt
        WORKING_DIRECTORY "${SOURCE_PATH}"
        LOGNAME "prepare-requirements-${RELEASE_TRIPLET}"
    )
endif()

file(INSTALL ${SOURCE_PATH}/wx/ DESTINATION ${CURRENT_PACKAGES_DIR}/tools/python3/Lib/site-packages/wx/)
file(INSTALL ${SOURCE_PATH}/wx/include/wxPython/ DESTINATION ${CURRENT_PACKAGES_DIR}/include/wxPython/)

# Per https://www.wxpython.org/pages/license/, using wxWindows Library License, no license in the source repo
file(INSTALL ${CMAKE_CURRENT_LIST_DIR}/copyright DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT})

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/tools/python3/Lib/site-packages/numpy/_core/lib/pkgconfig/")