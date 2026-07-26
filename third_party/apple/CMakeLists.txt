# apple-oss-distributions *_cmds — real BSD userland, built as individual
# executables and dropped into the initrd unsigned (TVM emulates them).

set(APPLE_CMDS_MODULES adv_cmds system_cmds network_cmds diskdev_cmds)
set(APPLE_CMDS_BUILD ${THIRDPARTY_BUILD}/apple)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(APPLE_CMDS_PLATFORM ios)
    set(APPLE_CMDS_ENV ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" TVM_IOS_CROSS=1)
else()
    set(APPLE_CMDS_PLATFORM macos)
    set(APPLE_CMDS_ENV ${CMAKE_COMMAND} -E env "CC=${TVM_CC}")
endif()

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
    message(FATAL_ERROR
        "third_party/apple/include is missing — run scripts/fetch_apple_headers.sh")
endif()

# Parse a .cmds manifest, return the command names for the current platform.
function(apple_cmds_list MODULE OUTVAR)
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/${MODULE}.cmds LINES)
    set(RESULT "")
    foreach(line IN LISTS LINES)
        if(NOT line MATCHES "\\|" OR line MATCHES "^[ \t]*#")
            continue()
        endif()
        string(REPLACE "|" ";" fields "${line}")
        list(GET fields 0 cmd)
        list(LENGTH fields nfields)
        set(platform "")
        if(nfields GREATER 4)
            list(GET fields 4 platform)
            string(STRIP "${platform}" platform)
        endif()
        if(platform STREQUAL "" OR platform STREQUAL "${APPLE_CMDS_PLATFORM}")
            list(APPEND RESULT ${cmd})
        endif()
    endforeach()
    set(${OUTVAR} ${RESULT} PARENT_SCOPE)
endfunction()

set(APPLE_CMDS_ALL "")

foreach(module ${APPLE_CMDS_MODULES})
    ExternalProject_Add(${module}_ext
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${module}
        BINARY_DIR ${APPLE_CMDS_BUILD}/${module}
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ${APPLE_CMDS_ENV} sh ${SCRIPTS_DIR}/build_apple_cmds.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/${module}
            ${APPLE_CMDS_BUILD}/${module}
            ${CMAKE_CURRENT_SOURCE_DIR}/${module}.cmds
        BUILD_ALWAYS TRUE
        INSTALL_COMMAND ""
        EXCLUDE_FROM_ALL TRUE
    )
    add_custom_target(${module}_exe DEPENDS ${module}_ext)
    set_target_properties(${module}_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

    apple_cmds_list(${module} cmds)
    list(APPEND APPLE_CMDS_ALL ${cmds})
    set(APPLE_CMDS_${module} "${cmds}" CACHE INTERNAL "")
endforeach()

add_custom_target(apple_cmds)
foreach(module ${APPLE_CMDS_MODULES})
    add_dependencies(apple_cmds ${module}_exe)
endforeach()
set_target_properties(apple_cmds PROPERTIES EXCLUDE_FROM_ALL TRUE)

set(APPLE_CMDS_MODULES ${APPLE_CMDS_MODULES} CACHE INTERNAL "")
set(APPLE_CMDS_BUILD ${APPLE_CMDS_BUILD} CACHE INTERNAL "")
