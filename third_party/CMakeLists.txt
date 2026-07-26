include(ExternalProject)

# unicorn (CPU emulator) — uses cmake natively
set(UNICORN_INTERPRETER ON CACHE BOOL "" FORCE)
set(UNICORN_ARCH "x86;aarch64" CACHE STRING "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Reduce TCG buffer from 1GB default — multiple emulator instances
# on iOS would exceed the process virtual memory limit.
add_definitions(-DDEFAULT_CODE_GEN_BUFFER_SIZE_1=8388608)
add_subdirectory(unicorn EXCLUDE_FROM_ALL)

set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../scripts)
set(THIRDPARTY_SRC ${CMAKE_CURRENT_SOURCE_DIR})
set(THIRDPARTY_BUILD ${CMAKE_CURRENT_BINARY_DIR})
set(LIB_OUT ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})
set(BIN_OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# Cross-compile marker for the shell-script builds (build_toybox.sh,
# build_vim.sh). These used to read TVM_IOS_CROSS straight out of the ambient
# environment, which build.py exports — but only while it is *configuring*.
# A later `ninja` run that rebuilds vim or toybox (a touched CMakeLists is
# enough) then ran them without it, and vim's configure would try to run an
# arm64 test binary on the build host and fail with "cannot run C compiled
# programs". Pin it into the build commands instead, the way the top-level
# flutter target already does, so it no longer depends on how ninja was reached.
set(TVM_CROSS_ENV "")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(TVM_CROSS_ENV TVM_IOS_CROSS=1)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(IOS_CC_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/ios-cc.sh)
    file(WRITE ${IOS_CC_SCRIPT} [=[#!/bin/sh
exec xcrun --sdk iphoneos cc -arch arm64 -miphoneos-version-min=15.0 "$@"
]=])
    file(CHMOD ${IOS_CC_SCRIPT} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    set(TVM_CC "${IOS_CC_SCRIPT}")
else()
    set(TVM_CC "cc")
endif()

# Launcher — tiny executable that execs a dylib via PATH
set(LAUNCHER_SRC ${SCRIPTS_DIR}/launcher.c)
function(add_launcher NAME)
    add_custom_target(${NAME}_launcher
        COMMAND cc -o ${BIN_OUT}/${NAME}_launcher
            ${LAUNCHER_SRC} -DTARGET=\\\"lib${NAME}.dylib\\\"
        COMMENT "Building ${NAME} launcher"
    )
    set_target_properties(${NAME}_launcher PROPERTIES EXCLUDE_FROM_ALL TRUE)
endfunction()

# Helper to add a _shared target that links .o files into a dylib
function(add_shared_lib NAME OBJGLOB)
    set(BUILDDIR ${THIRDPARTY_BUILD}/${NAME})
    set(LIBS "${ARGN}")
    string(REPLACE ";" " " LIBS_STR "${LIBS}")
    add_custom_target(${NAME}_shared
        COMMAND sh -c "${TVM_CC} -dynamiclib -fPIC -g -o ${LIB_OUT}/lib${NAME}.dylib ${BUILDDIR}/${OBJGLOB} ${LIBS_STR} -install_name lib${NAME}.dylib"
        DEPENDS ${NAME}_ext
        COMMENT "Linking ${NAME} (shared)"
    )
    set_target_properties(${NAME}_shared PROPERTIES EXCLUDE_FROM_ALL TRUE)
endfunction()

###############################################################################
# apple *_cmds (macOS / iOS only)
#
# The real BSD userland from apple-oss-distributions. Nothing here builds on
# Linux — the sources are Mach/Darwin-specific throughout — so the whole
# subdirectory is Apple-only.
###############################################################################
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_subdirectory(apple)
endif()

###############################################################################
# dropbear
###############################################################################
set(DROPBEAR_BUILD ${THIRDPARTY_BUILD}/dropbear)

set(DROPBEAR_CONFIGURE_ARGS
    "CFLAGS=-g -gdwarf-4"
    ac_cv_type_socklen_t=yes
    ac_cv_type_uid_t=yes
)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND DROPBEAR_CONFIGURE_ARGS
        --host=aarch64-apple-darwin
        "CC=${TVM_CC}"
    )
endif()

ExternalProject_Add(dropbear_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/dropbear
    BINARY_DIR ${DROPBEAR_BUILD}
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy
        ${THIRDPARTY_SRC}/dropbear/localoptions.h ${DROPBEAR_BUILD}/localoptions.h
    CONFIGURE_COMMAND ${THIRDPARTY_SRC}/dropbear/configure ${DROPBEAR_CONFIGURE_ARGS}
    BUILD_COMMAND make
        "PROGRAMS=dropbear dbclient dropbearkey scp"
        MULTI=1
        "CFLAGS=-g -gdwarf-4 -DDROPBEAR_FORKLESS"
        MAKEOVERRIDES=
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)
add_custom_target(dropbear_exe DEPENDS dropbear_ext)
set_target_properties(dropbear_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_shared_lib(dropbear "obj/*.o"
    ${DROPBEAR_BUILD}/libtomcrypt/libtomcrypt.a
    ${DROPBEAR_BUILD}/libtommath/libtommath.a -lz
)
add_launcher(dropbear)

###############################################################################
# mksh
###############################################################################
set(MKSH_BUILD ${THIRDPARTY_BUILD}/mksh)

ExternalProject_Add(mksh_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/mksh
    BINARY_DIR ${MKSH_BUILD}
    CONFIGURE_COMMAND sh -c "cd ${MKSH_BUILD} && \
        CC=${TVM_CC} CFLAGS='-g -gdwarf-4 -fcommon -DMKSH_FORKLESS=1' \
        sh ${THIRDPARTY_SRC}/mksh/Build.sh"
    BUILD_COMMAND sh -c "cd ${MKSH_BUILD} && sh ./Rebuild.sh && \
        ${TVM_CC} -g -gdwarf-4 -o mksh ./*.o"
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)
add_custom_target(mksh_exe DEPENDS mksh_ext)
set_target_properties(mksh_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_shared_lib(mksh "*.o")
add_launcher(mksh)

###############################################################################
# toybox
###############################################################################
set(TOYBOX_BUILD ${THIRDPARTY_BUILD}/toybox)

set(TOYBOX_LINK_LIBS -lm -lz)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # iOS: no openssl/crypt/util/resolv
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(COMMAND brew --prefix openssl
        OUTPUT_VARIABLE OPENSSL_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT OPENSSL_PREFIX)
        set(OPENSSL_PREFIX /opt/homebrew/opt/openssl)
    endif()
    list(APPEND TOYBOX_LINK_LIBS -lcrypto -lresolv -lssl -L${OPENSSL_PREFIX}/lib)
else()
    list(APPEND TOYBOX_LINK_LIBS -lcrypto -lresolv -lssl -lcrypt -lutil)
endif()

ExternalProject_Add(toybox_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/toybox
    BINARY_DIR ${TOYBOX_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" ${TVM_CROSS_ENV} sh ${SCRIPTS_DIR}/build_toybox.sh ${THIRDPARTY_SRC}/toybox ${TOYBOX_BUILD}
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)
add_custom_target(toybox_exe DEPENDS toybox_ext)
set_target_properties(toybox_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_shared_lib(toybox "generated/obj/*.o" ${TOYBOX_LINK_LIBS})
add_launcher(toybox)

# Host toybox — hermetic cpio tool for packing initrds. Built with the host
# compiler (not TVM_CC) so it runs on the build machine even when the target
# toybox is cross-compiled (e.g. iOS). mkinitrd uses its cpio for every initrd.
set(TOYBOX_HOST_BUILD ${THIRDPARTY_BUILD}/toybox_host)
ExternalProject_Add(toybox_host_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/toybox
    BINARY_DIR ${TOYBOX_HOST_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=cc" sh ${SCRIPTS_DIR}/build_toybox.sh ${THIRDPARTY_SRC}/toybox ${TOYBOX_HOST_BUILD}
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)

###############################################################################
# vim
###############################################################################
set(VIM_BUILD ${THIRDPARTY_BUILD}/vim)
set(STUBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../stubs)

set(VIM_LINK_LIBS -lm)
set(VIM_BUILD_ENV "")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND VIM_LINK_LIBS ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libtermcap_stubs.a)
    set(VIM_BUILD_ENV ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" "TVM_STUBS_DIR=${STUBS_DIR}" ${TVM_CROSS_ENV})
else()
    list(APPEND VIM_LINK_LIBS -lncurses)
endif()

ExternalProject_Add(vim_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/vim
    BINARY_DIR ${VIM_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${VIM_BUILD_ENV} sh ${SCRIPTS_DIR}/build_vim.sh ${THIRDPARTY_SRC}/vim ${VIM_BUILD}
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)
add_custom_target(vim_exe DEPENDS vim_ext)
set_target_properties(vim_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_shared_lib(vim "objects/*.o" ${VIM_LINK_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_dependencies(vim_shared termcap_stubs)
endif()
add_launcher(vim)
