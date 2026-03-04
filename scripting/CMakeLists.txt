set( KIPYTHON_SRCS
    kipython_settings.cpp
)

add_library( scripting STATIC
    ${KIPYTHON_SRCS}
)

target_link_libraries( scripting
    ${wxWidgets_LIBRARIES}      # wxLogDebug, wxASSERT
    ${PYTHON_LIBRARIES}
    Boost::headers
    common
    kicommon
)

target_include_directories( scripting PUBLIC
    ${PYTHON_INCLUDE_DIRS}
    ${PROJECT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories( scripting PRIVATE
    $<TARGET_PROPERTY:nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>
    ${PROJECT_SOURCE_DIR}/resources/bitmaps_png/include
    ${PROJECT_SOURCE_DIR}/include
    ${wxWidgets_LIBRARIES}
)

# Setup the KIFACE
add_library( scripting_kiface MODULE
    kicad_scripting_main.cpp
    kipython_frame.cpp
    ${KIPYTHON_SRCS}
    )

set_source_files_properties( kicad_scripting_main.cpp PROPERTIES
    # The KIFACE is in kicad_scripting_main.cpp, export it:
    COMPILE_DEFINITIONS     "BUILD_KIWAY_DLL;COMPILING_DLL"
    )

target_include_directories( scripting_kiface PRIVATE
    ${PROJECT_SOURCE_DIR}/resources/bitmaps_png/include
    ${PROJECT_SOURCE_DIR}/include
    ${wxWidgets_LIBRARIES}
)

if( MSVC )
    target_sources( scripting_kiface PRIVATE ${CMAKE_SOURCE_DIR}/resources/msw/kipython-dll.rc )
endif()

target_link_libraries( scripting_kiface
    scripting
    )

set_target_properties( scripting_kiface PROPERTIES
    OUTPUT_NAME     kipython
    PREFIX          ${KIFACE_PREFIX}
    SUFFIX          ${KIFACE_SUFFIX}
    )

target_link_options( scripting_kiface PRIVATE
    $<$<BOOL:${KICAD_MAKE_LINK_MAPS}>:-Wl,--cref,-Map=_scripting.kiface.map>
    )

if( APPLE )
    set_target_properties( scripting_kiface PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${OSX_BUNDLE_BUILD_KIFACE_DIR}
        )
    set_target_properties( scripting_kiface PROPERTIES INSTALL_RPATH
            "@executable_path/../Frameworks;@executable_path/../Frameworks/Python.framework" )
    set_target_properties( scripting_kiface PROPERTIES BUILD_WITH_INSTALL_RPATH 1 )

    install( CODE "
        set( KICAD_CMAKE_MODULE_PATH \"${KICAD_CMAKE_MODULE_PATH}\" )
        set( OSX_BUNDLE_BUILD_KIFACE_DIR \"${OSX_BUNDLE_BUILD_KIFACE_DIR}\" )
        set( OSX_BUNDLE_INSTALL_LIB_DIR \"${OSX_BUNDLE_INSTALL_LIB_DIR}\" )

        include( ${KICAD_CMAKE_MODULE_PATH}/InstallSteps/InstallMacOS.cmake )

        # Install any dependencies
        install_runtime_deps( \"\"
            \"${OSX_BUNDLE_BUILD_KIFACE_DIR}/_kipython.kiface\"
            \"\"
            )
    " )
else()
    install( TARGETS scripting_kiface
        DESTINATION ${KICAD_KIFACE}
        COMPONENT binary
        )
endif()

if( KICAD_WIN32_INSTALL_PDBS )
    # Get the PDBs to copy over for MSVC
    install(FILES $<TARGET_PDB_FILE:scripting_kiface> DESTINATION ${KICAD_KIFACE})
endif()

# python shell installation
install( DIRECTORY ${PROJECT_SOURCE_DIR}/scripting/kicad_pyshell/
    DESTINATION ${KICAD_DATA}/scripting/kicad_pyshell
    FILE_PERMISSIONS OWNER_EXECUTE OWNER_READ OWNER_WRITE GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
)

# Note: ai_local_client.py has been removed - replaced by AI_BACKEND_CLIENT C++ class
# Backend communication is now handled natively in C++ via ai_backend_client.cpp

# Copy requirements.txt for reference
configure_file( ${PROJECT_SOURCE_DIR}/scripts/requirements.txt 
               ${CMAKE_BINARY_DIR}/scripting/requirements.txt 
               COPYONLY )

# Copy trace module to build directory for development
file( COPY ${PROJECT_SOURCE_DIR}/trace/
      DESTINATION ${CMAKE_BINARY_DIR}/scripting/trace/
      FILES_MATCHING PATTERN "*.py"
      PATTERN ".gitignore" EXCLUDE
      PATTERN "__pycache__" EXCLUDE
      PATTERN "*.pyc" EXCLUDE )

# Enhanced Python management for Windows vcpkg compatibility
if( WIN32 AND VCPKG_INSTALLED_DIR )
    set( VCPKG_PYTHON_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/python3" )
    
    # Check if vcpkg Python exists and lacks pip
    if( EXISTS "${VCPKG_PYTHON_DIR}/python.exe" )
        message( STATUS "Found vcpkg Python at: ${VCPKG_PYTHON_DIR}" )
        
        # Pre-install Python packages during build to avoid runtime dependency issues
        # Find system Python with pip for package installation
        find_program( SYSTEM_PYTHON_WITH_PIP 
            NAMES python python3 python.exe python3.exe
            HINTS "C:/Python*" "$ENV{LOCALAPPDATA}/Programs/Python/Python*" 
                  "$ENV{APPDATA}/Python/Python*" "$ENV{PROGRAMFILES}/Python*"
        )
        
        if( SYSTEM_PYTHON_WITH_PIP )
            # Test if system Python has pip
            execute_process(
                COMMAND "${SYSTEM_PYTHON_WITH_PIP}" -m pip --version
                RESULT_VARIABLE PIP_TEST_RESULT
                OUTPUT_QUIET ERROR_QUIET
            )
            
            if( PIP_TEST_RESULT EQUAL 0 )
                message( STATUS "System Python with pip found: ${SYSTEM_PYTHON_WITH_PIP}" )
                
                # Install packages to a local directory that we can bundle
                set( LOCAL_PACKAGES_DIR "${CMAKE_BINARY_DIR}/python_packages" )
                
                execute_process(
                    COMMAND "${SYSTEM_PYTHON_WITH_PIP}" -m pip install 
                        --target "${LOCAL_PACKAGES_DIR}"
                        --quiet python-dotenv httpx httpx-sse
                    RESULT_VARIABLE BUNDLE_INSTALL_RESULT
                    OUTPUT_QUIET ERROR_QUIET
                )
                
                if( BUNDLE_INSTALL_RESULT EQUAL 0 )
                    message( STATUS "Pre-installed Python packages to: ${LOCAL_PACKAGES_DIR}" )
                    
                    # Install the bundled packages
                    install( DIRECTORY "${LOCAL_PACKAGES_DIR}/"
                        DESTINATION ${KICAD_DATA}/scripting/python_packages
                        FILES_MATCHING 
                        PATTERN "*.py"
                        PATTERN "*.pyd" 
                        PATTERN "*.dll"
                        PATTERN "__pycache__" EXCLUDE
                        PATTERN "*.pyc" EXCLUDE
                    )
                else()
                    message( WARNING "Failed to pre-install Python packages for bundling" )
                endif()
            endif()
        endif()
    endif()
endif()

install( CODE "
    message( STATUS \"Installing Python dependencies for local client...\" )
    
    # Try to install dependencies with available Python
    set( PYTHON_CANDIDATES \"${PYTHON_EXECUTABLE}\" )
    if( WIN32 )
        list( APPEND PYTHON_CANDIDATES \"python\" \"python3\" )
    endif()
    
    set( PIP_INSTALL_SUCCESS FALSE )
    foreach( PYTHON_CMD IN LISTS PYTHON_CANDIDATES )
        execute_process(
            COMMAND \${PYTHON_CMD} -m pip --version
            RESULT_VARIABLE PIP_CHECK_RESULT
            OUTPUT_QUIET ERROR_QUIET
        )
        
        if( PIP_CHECK_RESULT EQUAL 0 )
            execute_process(
                COMMAND \${PYTHON_CMD} -m pip install --quiet python-dotenv httpx httpx-sse
                RESULT_VARIABLE PIP_INSTALL_RESULT
                OUTPUT_QUIET ERROR_QUIET
            )
            
            if( PIP_INSTALL_RESULT EQUAL 0 )
                message( STATUS \"  ✓ Installed python-dotenv, httpx, httpx-sse using \${PYTHON_CMD}\" )
                set( PIP_INSTALL_SUCCESS TRUE )
                break()
            endif()
        endif()
    endforeach()
    
    if( NOT PIP_INSTALL_SUCCESS )
        message( WARNING \"Failed to install Python dependencies - AI features may require manual dependency installation\" )
        message( STATUS \"  Required packages: python-dotenv, httpx, httpx-sse\" )
        message( STATUS \"  Install with: pip install python-dotenv httpx httpx-sse\" )
    endif()
" )

# Trace converter installation (for AI tool execution - trace to KiCad format conversion)
install( DIRECTORY ${PROJECT_SOURCE_DIR}/trace/
    DESTINATION ${KICAD_DATA}/scripting/trace
    FILE_PERMISSIONS OWNER_EXECUTE OWNER_READ OWNER_WRITE GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
    PATTERN ".gitignore" EXCLUDE
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    PATTERN "*.log" EXCLUDE
    PATTERN "output.*" EXCLUDE
)

# TODO: Python bytecode compilation (.pyc) for distribution
# Currently disabled due to codesigning/install order issues
# The compiled .pyc files are being deleted during the install/signing process
# For now, source .py files are shipped with path validation security in place


