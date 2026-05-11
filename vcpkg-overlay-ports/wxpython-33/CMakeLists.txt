cmake_minimum_required(VERSION 3.14)
# Minimum 3.12 required for CONFIGURE_DEPENDS

project( wxpython )

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(WXPYTHON_SIP_DIR ${PROJECT_SOURCE_DIR}/sip)
set(WXPYTHON_SRC_DIR ${PROJECT_SOURCE_DIR}/src)
set(WXPYTHON_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/src)
set(WXPYTHON_WXP_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/wx/include)
set(WXPYTHON_OUT_DIR ${PROJECT_SOURCE_DIR}/wx)


add_definitions(-DSIP_MODULE_NAME=wx.siplib -DSIP_MODULE_BASENAME=siplib)
add_definitions(-DUNICODE -D_UNICODE -DUTF8)
add_definitions(-D_WINDOWS -DWIN32 -D__WXMSW__ -DWXUSINGDLL=1 -DISOLATION_AWARE_ENABLED)

if(VCPKG_TOOLCHAIN)
    #This is a silly hack to convince the cmake helper to look in /debug/lib for the debug library
    #or else it wont it
    set( Python3_FIND_REGISTRY NEVER )
    foreach(root in ${CMAKE_FIND_ROOT_PATH})
        list(APPEND PYTHON3_ROOT_HINTS "${root}/lib")
    endforeach()
    set( Python3_ROOT_DIR  ${PYTHON3_ROOT_HINTS} )
endif()
find_package( Python3 COMPONENTS Development REQUIRED )

find_package( wxWidgets COMPONENTS gl aui adv html core net base propgrid media xml ribbon richtext webview xrc stc REQUIRED CONFIG )

set(SIPLIB_SRCS 
    ${WXPYTHON_SIP_DIR}/siplib/apiversions.c
    ${WXPYTHON_SIP_DIR}/siplib/descriptors.c
    ${WXPYTHON_SIP_DIR}/siplib/int_convertors.c
    ${WXPYTHON_SIP_DIR}/siplib/objmap.c
    ${WXPYTHON_SIP_DIR}/siplib/qtlib.c
    ${WXPYTHON_SIP_DIR}/siplib/sip_array.c
    ${WXPYTHON_SIP_DIR}/siplib/siplib.c
    ${WXPYTHON_SIP_DIR}/siplib/threads.c
    ${WXPYTHON_SIP_DIR}/siplib/voidptr.c
)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(OUTPUT_PREFIX "_d.pyd")
else()
    set(OUTPUT_PREFIX ".pyd")
endif()

add_library(siplib SHARED ${SIPLIB_SRCS})
target_include_directories( siplib PUBLIC ${Python3_INCLUDE_DIRS} )
target_link_libraries( siplib PUBLIC ${Python3_LIBRARIES} )
set_target_properties( siplib PROPERTIES 
    OUTPUT_NAME siplib 
    SUFFIX      ${OUTPUT_PREFIX}
    )
install(TARGETS siplib RUNTIME DESTINATION ${WXPYTHON_OUT_DIR})

function(build_wxp_pyd lib_name glob)
    file(COPY ${WXPYTHON_SRC_DIR}/wxc.rc DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
    file(RENAME ${CMAKE_CURRENT_BINARY_DIR}/wxc.rc ${CMAKE_CURRENT_BINARY_DIR}/_${lib_name}.rc)

    if(NOT ${glob} STREQUAL "")
        file(GLOB SRCS ${glob} )
    else()
        file(GLOB SRCS ${WXPYTHON_SIP_DIR}/cpp/sip_${lib_name}*.cpp )
    endif()

    add_library( ${lib_name} SHARED ${SRCS} ${CMAKE_CURRENT_BINARY_DIR}/_${lib_name}.rc )
    target_include_directories( ${lib_name} PUBLIC ${WXPYTHON_SIP_DIR}/siplib ${WXPYTHON_SRC_DIR} ${WXPYTHON_WXP_INCLUDE_DIR} ${Python3_INCLUDE_DIRS} ${wxWidgets_INCLUDE_DIRS} )
    add_dependencies( ${lib_name} siplib )


    target_link_libraries( ${lib_name}
                            siplib 
                            ${Python3_LIBRARIES} 
                            ${wxWidgets_LIBRARIES})
                            

    set_target_properties( ${lib_name} PROPERTIES 
        OUTPUT_NAME _${lib_name}
        SUFFIX      ${OUTPUT_PREFIX}
    )

    install(TARGETS ${lib_name} RUNTIME DESTINATION ${WXPYTHON_OUT_DIR})
endfunction()

build_wxp_pyd(core "")
build_wxp_pyd(adv "")
build_wxp_pyd(aui "")
build_wxp_pyd(dataview "")
build_wxp_pyd(glcanvas "")
build_wxp_pyd(grid "")
build_wxp_pyd(html "${WXPYTHON_SIP_DIR}/cpp/sip_html[!2]*.cpp")
build_wxp_pyd(html2 "")
build_wxp_pyd(media "")
build_wxp_pyd(msw "")
build_wxp_pyd(propgrid "")
build_wxp_pyd(ribbon "")
build_wxp_pyd(richtext "")
build_wxp_pyd(stc "")
build_wxp_pyd(xml "")
build_wxp_pyd(xrc "")


install(FILES ${WXPYTHON_SRC_DIR}/__init__.py
            DESTINATION ${WXPYTHON_OUT_DIR})
