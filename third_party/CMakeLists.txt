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
# unicorn otherwise forces a universal (x86_64;arm64) binary on every Apple
# platform. Build it non-fat for the single target arch -- the unused slice is
# wasted work, and on iOS the x86_64 slice no longer compiles (the iPhoneOS SDK
# dropped x86_64 headers). unicorn honours ARCHFLAGS; use the explicit target
# arch (set for iOS) or the host arch (native macOS). Non-Apple hosts need
# nothing -- unicorn's fat logic is Apple-only, so linux-x64 is native already.
if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES)
        set(ENV{ARCHFLAGS} "-arch ${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(ENV{ARCHFLAGS} "-arch ${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
endif()
add_subdirectory(unicorn EXCLUDE_FROM_ALL)

set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../scripts)

# tvm_ext_inputs(<ExternalProject> <input>...)
# Re-run the project's build step when an input changes. ExternalProject
# considers a step done for good once its stamp exists, so without this an
# edited build script, TVM config or shim file, or a submodule moved to another
# revision never reached an existing build tree. Plain paths are files;
# SUBMODULE:<path> is the submodule's checked-out revision, tracked through its
# HEAD file under .git/modules (rewritten by every `git submodule update`).
# The build scripts then decide from the same inputs whether to rebuild from
# scratch (scripts/tvm_stamp.sh).
#
# Only list sources here, never another ExternalProject's outputs (libcurl.a,
# libncursesw.a, ...): no rule declares those, so ninja fails on a fresh tree
# with "missing and no known rule to make it". A rebuilt dependency is already
# covered by DEPENDS <project>, whose done stamp ExternalProject makes a file
# dependency of this project's configure step.
function(tvm_ext_inputs target)
    set(deps ${SCRIPTS_DIR}/tvm_stamp.sh)
    foreach(item ${ARGN})
        if(item MATCHES "^SUBMODULE:(.*)$")
            set(head "${CMAKE_SOURCE_DIR}/.git/modules/${CMAKE_MATCH_1}/HEAD")
            if(EXISTS "${head}")
                list(APPEND deps "${head}")
            endif()
        else()
            list(APPEND deps "${item}")
        endif()
    endforeach()
    ExternalProject_Add_StepDependencies(${target} build ${deps})
endfunction()
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
        COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_OUT}
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
        ${CMAKE_CURRENT_SOURCE_DIR}/configs/dropbear_localoptions.h ${DROPBEAR_BUILD}/localoptions.h
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
# sftp-server (standalone)
###############################################################################
set(SFTPSERVER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sftp-server.c)
set(SFTPSERVER_OBJ ${THIRDPARTY_BUILD}/sftpserver/sftp-server.o)
set(SFTPSERVER_EXE ${THIRDPARTY_BUILD}/sftpserver/sftp-server)
add_custom_target(sftpserver_ext
    COMMAND ${CMAKE_COMMAND} -E make_directory ${THIRDPARTY_BUILD}/sftpserver
    COMMAND ${TVM_CC} -g -gdwarf-4 -c ${SFTPSERVER_SRC} -o ${SFTPSERVER_OBJ}
    COMMAND ${TVM_CC} -g -o ${SFTPSERVER_EXE} ${SFTPSERVER_OBJ}
    COMMENT "Building sftp-server"
)
set_target_properties(sftpserver_ext PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_custom_target(sftpserver_shared
    COMMAND ${TVM_CC} -dynamiclib -fPIC -g -o ${LIB_OUT}/libsftpserver.dylib
        ${SFTPSERVER_OBJ} -install_name libsftpserver.dylib
    DEPENDS sftpserver_ext
    COMMENT "Linking sftp-server (shared)"
)
set_target_properties(sftpserver_shared PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_launcher(sftpserver)

###############################################################################
# bash
#
# Unpatched GNU bash, autotools out-of-tree build driven by
# scripts/build_bash.sh (configure cache vars + -D overrides on CFLAGS, no
# source patches). The exe is bash's own `bash`; the Flutter dylib relinks the
# objects of bash's link line (exported to objects/ by the script) with its
# static sub-libraries in bash's own order.
###############################################################################
set(BASH_BUILD ${THIRDPARTY_BUILD}/bash)

# Termcap: ncurses via the SDK's libtermcap.tbd on macOS, bash's bundled GNU
# termcap on iOS (no terminfo database on the device; see build_bash.sh).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(BASH_TERMCAP_LIB ${BASH_BUILD}/lib/termcap/libtermcap.a)
else()
    set(BASH_TERMCAP_LIB -ltermcap)
endif()
set(BASH_LINK_LIBS
    ${BASH_BUILD}/builtins/libbuiltins.a
    ${BASH_BUILD}/lib/glob/libglob.a
    ${BASH_BUILD}/lib/sh/libsh.a
    ${BASH_BUILD}/lib/readline/libreadline.a
    ${BASH_BUILD}/lib/readline/libhistory.a
    ${BASH_TERMCAP_LIB}
    ${BASH_BUILD}/lib/tilde/libtilde.a
)

ExternalProject_Add(bash_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/bash
    BINARY_DIR ${BASH_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" ${TVM_CROSS_ENV} sh ${SCRIPTS_DIR}/build_bash.sh ${THIRDPARTY_SRC}/bash ${BASH_BUILD}
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)
tvm_ext_inputs(bash_ext ${SCRIPTS_DIR}/build_bash.sh SUBMODULE:third_party/bash
    ${CMAKE_CURRENT_SOURCE_DIR}/configs/bash_stdio_unlocked.h)
add_custom_target(bash_exe DEPENDS bash_ext)
set_target_properties(bash_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_shared_lib(bash "objects/*.o" ${BASH_LINK_LIBS})
add_launcher(bash)

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
# Re-run the build step when the applet config or the build script changes;
# ExternalProject otherwise considers the step done forever once its stamp
# exists, so enabling an applet in toybox.config never reached an existing
# build tree.
ExternalProject_Add_StepDependencies(toybox_ext build
    ${THIRDPARTY_SRC}/configs/toybox.config ${SCRIPTS_DIR}/build_toybox.sh)
tvm_ext_inputs(toybox_ext SUBMODULE:third_party/toybox ${CMAKE_CURRENT_SOURCE_DIR}/include-shim/tvm_ios_libc.h)
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
ExternalProject_Add_StepDependencies(toybox_host_ext build
    ${THIRDPARTY_SRC}/configs/toybox.config ${SCRIPTS_DIR}/build_toybox.sh)
tvm_ext_inputs(toybox_host_ext SUBMODULE:third_party/toybox)

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

###############################################################################
# openssl (static libssl/libcrypto for curl)
###############################################################################
set(OPENSSL_BUILD ${THIRDPARTY_BUILD}/openssl)
set(TVM_OPENSSL_PREFIX ${OPENSSL_BUILD}/prefix)

# Configure target. ios64-xcrun already compiles with `xcrun -sdk iphoneos cc
# -arch arm64`; the extra -miphoneos-version-min matches ios-cc.sh.
set(OPENSSL_CONFIGURE_EXTRA "")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(OPENSSL_TARGET ios64-xcrun)
    set(OPENSSL_CONFIGURE_EXTRA -miphoneos-version-min=15.0)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(OPENSSL_TARGET darwin64-arm64-cc)
else()
    set(OPENSSL_TARGET linux-x86_64)
endif()

cmake_host_system_information(RESULT TVM_NPROC QUERY NUMBER_OF_LOGICAL_CORES)

ExternalProject_Add(openssl_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/openssl
    BINARY_DIR ${OPENSSL_BUILD}
    CONFIGURE_COMMAND perl ${THIRDPARTY_SRC}/openssl/Configure ${OPENSSL_TARGET}
        --prefix=${TVM_OPENSSL_PREFIX} --libdir=lib
        no-shared no-dso no-module no-engine no-dynamic-engine no-legacy no-fips
        no-async no-thread-pool no-default-thread-pool no-secure-memory no-atexit
        no-apps no-tests no-docs no-sctp no-ktls --with-rand-seed=os
        ${OPENSSL_CONFIGURE_EXTRA} -g -gdwarf-4
    BUILD_COMMAND make -j${TVM_NPROC} build_libs
    INSTALL_COMMAND make install_dev
    EXCLUDE_FROM_ALL TRUE
)

###############################################################################
# curl
#
# Its own CMake builds libcurl.a and compiles the tool objects; the tool is
# then relinked by scripts/build_curl_tool.sh with the TVM main() wrapper
# (third_party/curl_main.c), as a static executable for the CLI initrd and as
# libcurl.dylib (exporting main) for the Flutter initrd.
###############################################################################
set(CURL_BUILD ${THIRDPARTY_BUILD}/curl)
set(CURL_TOOL_BUILD ${THIRDPARTY_BUILD}/curl_tool)

set(CURL_CMAKE_ARGS
    "-DCMAKE_C_FLAGS=-g -gdwarf-4"
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_STATIC_CURL=ON
    -DBUILD_CURL_EXE=ON
    -DCURL_USE_OPENSSL=ON
    -DOPENSSL_ROOT_DIR=${TVM_OPENSSL_PREFIX}
    -DOPENSSL_USE_STATIC_LIBS=TRUE
    -DOPENSSL_INCLUDE_DIR=${TVM_OPENSSL_PREFIX}/include
    -DOPENSSL_SSL_LIBRARY=${TVM_OPENSSL_PREFIX}/lib/libssl.a
    -DOPENSSL_CRYPTO_LIBRARY=${TVM_OPENSSL_PREFIX}/lib/libcrypto.a
    -DCURL_CA_PATH=none
    -DCURL_DISABLE_OPENSSL_AUTO_LOAD_CONFIG=ON
    -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DUSE_APPLE_IDN=OFF -DUSE_NGHTTP2=OFF
    -DCURL_USE_LIBSSH2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ZLIB=ON
    -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON
    -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_TELNET=ON
    -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_GOPHER=ON
    -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_POP3=ON -DCURL_DISABLE_SMTP=ON
    -DENABLE_THREADED_RESOLVER=ON -DENABLE_IPV6=ON -DENABLE_UNIX_SOCKETS=OFF
    -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
    -DBUILD_LIBCURL_DOCS=OFF -DENABLE_CURL_MANUAL=OFF
)
if(APPLE)
    # Certificate verification through the OS trust store: no CA bundle needed
    # on macOS/iOS. (curl only enables SecTrust when no CA file is configured,
    # so nothing may set a CA bundle on Apple.)
    list(APPEND CURL_CMAKE_ARGS -DUSE_APPLE_SECTRUST=ON -DCURL_CA_BUNDLE=none)
else()
    # Linux: Mozilla's bundle (third_party/configs/cacert.pem) is shipped in
    # the initrd; make it curl's compile-time default.
    list(APPEND CURL_CMAKE_ARGS -DCURL_CA_BUNDLE=/tvm/etc/ssl/cacert.pem)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND CURL_CMAKE_ARGS
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
        -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_OSX_SYSROOT=iphoneos)
endif()

ExternalProject_Add(curl_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/curl
    BINARY_DIR ${CURL_BUILD}
    CMAKE_ARGS ${CURL_CMAKE_ARGS}
    INSTALL_COMMAND ""
    DEPENDS openssl_ext
    EXCLUDE_FROM_ALL TRUE
)

add_custom_target(curl_exe
    COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" ${TVM_CROSS_ENV} sh ${SCRIPTS_DIR}/build_curl_tool.sh
        exe ${CURL_BUILD} ${TVM_OPENSSL_PREFIX} ${CURL_TOOL_BUILD} ${CURL_TOOL_BUILD}/curl
    DEPENDS curl_ext
    COMMENT "Linking curl (TVM main wrapper)"
)
set_target_properties(curl_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_custom_target(curl_shared
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LIB_OUT}
    COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" ${TVM_CROSS_ENV} sh ${SCRIPTS_DIR}/build_curl_tool.sh
        dylib ${CURL_BUILD} ${TVM_OPENSSL_PREFIX} ${CURL_TOOL_BUILD} ${LIB_OUT}/libcurl.dylib
    DEPENDS curl_ext
    COMMENT "Linking curl (shared)"
)
set_target_properties(curl_shared PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_launcher(curl)

###############################################################################
# ncurses (static libncursesw for nano and less, plus a host-side tic)
#
# Unpatched upstream, configured by scripts/build_ncurses.sh. The target
# library goes into ${TVM_NCURSES_PREFIX}; the host build only provides
# progs/tic, which scripts/CMakeLists.txt runs at initrd time to compile the
# terminfo database shipped at /tvm/share/terminfo.
###############################################################################
set(NCURSES_BUILD ${THIRDPARTY_BUILD}/ncurses)
set(NCURSES_HOST_BUILD ${THIRDPARTY_BUILD}/ncurses_host)
set(TVM_NCURSES_PREFIX ${NCURSES_BUILD}/prefix)
set(NCURSES_LIB ${TVM_NCURSES_PREFIX}/lib/libncursesw.a)

ExternalProject_Add(ncurses_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/ncurses
    BINARY_DIR ${NCURSES_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" ${TVM_CROSS_ENV} sh ${SCRIPTS_DIR}/build_ncurses.sh target ${THIRDPARTY_SRC}/ncurses ${NCURSES_BUILD}
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)
ExternalProject_Add_StepDependencies(ncurses_ext build ${SCRIPTS_DIR}/build_ncurses.sh)
tvm_ext_inputs(ncurses_ext SUBMODULE:third_party/ncurses)

# Host tic: built with the host compiler so it runs on the build machine for
# every target (cf. toybox_host_ext).
ExternalProject_Add(ncurses_host_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/ncurses
    BINARY_DIR ${NCURSES_HOST_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=cc" sh ${SCRIPTS_DIR}/build_ncurses.sh host ${THIRDPARTY_SRC}/ncurses ${NCURSES_HOST_BUILD}
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)
ExternalProject_Add_StepDependencies(ncurses_host_ext build ${SCRIPTS_DIR}/build_ncurses.sh)
tvm_ext_inputs(ncurses_host_ext SUBMODULE:third_party/ncurses)

###############################################################################
# less
#
# Unpatched GNU less, generated sources + configure in a shadow tree
# (scripts/build_less.sh), linked against the static ncursesw above. The exe
# is less's own `less`; the Flutter dylib relinks the objects of less's link
# line (exported to objects/ by the script).
###############################################################################
set(LESS_BUILD ${THIRDPARTY_BUILD}/less)

ExternalProject_Add(less_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/less
    BINARY_DIR ${LESS_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" ${TVM_CROSS_ENV} sh ${SCRIPTS_DIR}/build_less.sh ${THIRDPARTY_SRC}/less ${LESS_BUILD} ${TVM_NCURSES_PREFIX}
    INSTALL_COMMAND ""
    DEPENDS ncurses_ext
    EXCLUDE_FROM_ALL TRUE
)
ExternalProject_Add_StepDependencies(less_ext build ${SCRIPTS_DIR}/build_less.sh)
tvm_ext_inputs(less_ext SUBMODULE:third_party/less ${CMAKE_CURRENT_SOURCE_DIR}/include-shim/tvm_ios_libc.h)
add_custom_target(less_exe DEPENDS less_ext)
set_target_properties(less_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_shared_lib(less "objects/*.o" ${NCURSES_LIB})
add_launcher(less)

###############################################################################
# nano
#
# Unpatched GNU nano built from git: gnulib import (third_party/gnulib, the
# commit nano's autogen.sh pins) and autoreconf in a shadow tree, out-of-tree
# configure and make (scripts/build_nano.sh), linked against the static
# ncursesw above. The exe is nano's own `nano`; the Flutter dylib relinks
# nano's objects (src/ holds nothing else) with gnulib's libgnu.a.
###############################################################################
set(NANO_BUILD ${THIRDPARTY_BUILD}/nano)

ExternalProject_Add(nano_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/nano
    BINARY_DIR ${NANO_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" ${TVM_CROSS_ENV} sh ${SCRIPTS_DIR}/build_nano.sh ${THIRDPARTY_SRC}/nano ${NANO_BUILD} ${THIRDPARTY_SRC}/gnulib ${TVM_NCURSES_PREFIX}
    INSTALL_COMMAND ""
    DEPENDS ncurses_ext
    EXCLUDE_FROM_ALL TRUE
)
ExternalProject_Add_StepDependencies(nano_ext build ${SCRIPTS_DIR}/build_nano.sh)
tvm_ext_inputs(nano_ext SUBMODULE:third_party/nano SUBMODULE:third_party/gnulib
    ${CMAKE_CURRENT_SOURCE_DIR}/configs/nano_ios.cache)
add_custom_target(nano_exe DEPENDS nano_ext)
set_target_properties(nano_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_shared_lib(nano "build/src/*.o" ${NANO_BUILD}/build/lib/libgnu.a ${NCURSES_LIB})
add_launcher(nano)

###############################################################################
# sqlite3 (command-line shell)
#
# Unpatched upstream, amalgamation (sqlite3.c + shell.c) generated by sqlite's
# own configure/make in gen/ with the host compiler and its in-tree JimTCL,
# then compiled for the target with an explicit define set together with
# linenoise (third_party/linenoise) for line editing -- scripts/build_sqlite.sh
# documents every flag. The exe is `sqlite3`; the Flutter dylib relinks the
# three objects. The target/dylib is named `sqlite` (libsqlite.dylib) rather
# than libsqlite3.dylib, the name of the system library on macOS and iOS.
###############################################################################
set(SQLITE_BUILD ${THIRDPARTY_BUILD}/sqlite)

ExternalProject_Add(sqlite_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/sqlite
    BINARY_DIR ${SQLITE_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" ${TVM_CROSS_ENV} sh ${SCRIPTS_DIR}/build_sqlite.sh ${THIRDPARTY_SRC}/sqlite ${SQLITE_BUILD} ${THIRDPARTY_SRC}/linenoise
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)
ExternalProject_Add_StepDependencies(sqlite_ext build ${SCRIPTS_DIR}/build_sqlite.sh)
tvm_ext_inputs(sqlite_ext SUBMODULE:third_party/sqlite SUBMODULE:third_party/linenoise
    ${CMAKE_CURRENT_SOURCE_DIR}/include-shim/tvm_ios_libc.h)
add_custom_target(sqlite_exe DEPENDS sqlite_ext)
set_target_properties(sqlite_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_shared_lib(sqlite "objects/*.o" -lm -lz)
add_launcher(sqlite)

###############################################################################
# git
#
# Unpatched upstream built in a shadow tree by scripts/build_git.sh with the
# knobs in third_party/configs/git_config.mak (its config.mak); HTTPS through
# the static libcurl/OpenSSL of the curl tool above. One multi-call image
# (third_party/git_main.c dispatches on argv[0]) serves git and the helpers
# git execs from its exec path (git-remote-https, git-http-fetch): the exe
# `git` for the CLI initrd, libgit.dylib for the Flutter initrd, with the
# helper names linked to /tvm/bin/git in /tvm/libexec/git-core by
# scripts/CMakeLists.txt. The script relinks the dylib itself (mode `dylib`,
# as build_curl_tool.sh does) because the link line is git's own, exported by
# the build step, rather than an object glob.
###############################################################################
set(GIT_BUILD ${THIRDPARTY_BUILD}/git)

ExternalProject_Add(git_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/git
    BINARY_DIR ${GIT_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" ${TVM_CROSS_ENV} sh ${SCRIPTS_DIR}/build_git.sh
        build ${THIRDPARTY_SRC}/git ${GIT_BUILD} ${THIRDPARTY_SRC}/curl ${CURL_BUILD} ${TVM_OPENSSL_PREFIX}
    INSTALL_COMMAND ""
    DEPENDS curl_ext
    EXCLUDE_FROM_ALL TRUE
)
ExternalProject_Add_StepDependencies(git_ext build
    ${SCRIPTS_DIR}/build_git.sh ${THIRDPARTY_SRC}/configs/git_config.mak ${THIRDPARTY_SRC}/git_main.c)
tvm_ext_inputs(git_ext SUBMODULE:third_party/git ${CMAKE_CURRENT_SOURCE_DIR}/include-shim/tvm_stdio_unlocked.h)
add_custom_target(git_exe DEPENDS git_ext)
set_target_properties(git_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_custom_target(git_shared
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LIB_OUT}
    COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" ${TVM_CROSS_ENV} sh ${SCRIPTS_DIR}/build_git.sh
        dylib ${THIRDPARTY_SRC}/git ${GIT_BUILD} ${THIRDPARTY_SRC}/curl ${CURL_BUILD} ${TVM_OPENSSL_PREFIX} ${LIB_OUT}/libgit.dylib
    DEPENDS git_ext
    COMMENT "Linking git (shared)"
)
set_target_properties(git_shared PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_launcher(git)
