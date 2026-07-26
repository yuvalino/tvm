# apple-oss-distributions *_cmds — the real BSD userland from Apple's own
# open-source drops, built as multi-call modules (see scripts/build_apple_cmds.sh).
#
# Included from ../CMakeLists.txt on Apple platforms only. Unlike the other
# third_party modules these produce plain executables rather than dylib +
# launcher pairs: they go into both initrds as real, unsigned binaries and run
# under TVM's emulator, so there is nothing to bundle into the app or codesign.
#
# Which commands each module builds is declared in <module>.cmds next to this
# file. Everything the SDKs do not declare comes from include/ and
# include-override/, refreshed by scripts/fetch_apple_headers.sh.

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

# Read a <module>.cmds manifest and return the command names that apply to the
# platform being built. Used here for stamp files and by scripts/CMakeLists.txt
# to lay down one symlink per command in the initrd.
function(apple_cmds_read MODULE OUTVAR)
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/${MODULE}.cmds LINES)
    set(RESULT "")
    foreach(line IN LISTS LINES)
        # A command row always has the '|' separators; anything else is a
        # comment, a blank line, or a #common/#cflags/#libs directive.
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
set(APPLE_CMDS_BINARIES "")

foreach(module ${APPLE_CMDS_MODULES})
    set(manifest ${CMAKE_CURRENT_SOURCE_DIR}/${module}.cmds)

    ExternalProject_Add(${module}_ext
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${module}
        BINARY_DIR ${APPLE_CMDS_BUILD}/${module}
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ${APPLE_CMDS_ENV} sh ${SCRIPTS_DIR}/build_apple_cmds.sh
            ${module} ${CMAKE_CURRENT_SOURCE_DIR}/${module}
            ${APPLE_CMDS_BUILD}/${module} ${manifest}
        BUILD_ALWAYS TRUE
        INSTALL_COMMAND ""
        EXCLUDE_FROM_ALL TRUE
    )

    add_custom_target(${module}_exe DEPENDS ${module}_ext)
    set_target_properties(${module}_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

    apple_cmds_read(${module} cmds)
    list(APPEND APPLE_CMDS_ALL ${cmds})
    list(APPEND APPLE_CMDS_BINARIES ${APPLE_CMDS_BUILD}/${module}/${module})

    # Each command is reachable as <module>/<cmd>; the initrd symlinks them.
    set(APPLE_CMDS_${module} ${cmds} CACHE INTERNAL "commands in ${module}")
endforeach()

# Convenience: build every module at once.
add_custom_target(apple_cmds)
foreach(module ${APPLE_CMDS_MODULES})
    add_dependencies(apple_cmds ${module}_exe)
endforeach()
set_target_properties(apple_cmds PROPERTIES EXCLUDE_FROM_ALL TRUE)

set(APPLE_CMDS_MODULES ${APPLE_CMDS_MODULES} CACHE INTERNAL "apple *_cmds modules")
set(APPLE_CMDS_BUILD ${APPLE_CMDS_BUILD} CACHE INTERNAL "apple *_cmds build dir")

list(LENGTH APPLE_CMDS_ALL n_apple_cmds)
message(STATUS "apple *_cmds: ${n_apple_cmds} commands for ${APPLE_CMDS_PLATFORM}")
