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
elseif(ANDROID)
    # Userland binaries run inside TVM; build them against a higher API level
    # than the VM itself (android-21) for broader libc coverage.
    set(ANDROID_CC_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/android-cc.sh)
    file(WRITE ${ANDROID_CC_SCRIPT} "#!/bin/sh
exec ${CMAKE_C_COMPILER} --target=aarch64-none-linux-android28 \"$@\"
")
    file(CHMOD ${ANDROID_CC_SCRIPT} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    set(TVM_CC "${ANDROID_CC_SCRIPT}")
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
set(DROPBEAR_MAKE_LDFLAGS)
set(DROPBEAR_MAKE_CFLAGS "")
set(DROPBEAR_LOCALOPTIONS_CMD ${CMAKE_COMMAND} -E copy
    ${THIRDPARTY_SRC}/dropbear/localoptions.h ${DROPBEAR_BUILD}/localoptions.h)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND DROPBEAR_CONFIGURE_ARGS
        --host=aarch64-apple-darwin
        "CC=${TVM_CC}"
    )
elseif(ANDROID)
    # Bionic has no crypt()/getpass(); provide link stubs (auth uses
    # DROPBEAR_EMBEDDED_PASSWORD, the stub result is never trusted) plus a
    # force-included header so their declarations are visible under
    # -Werror=implicit-function-declaration.
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/dropbear_android_stubs.c [=[
#include <stdio.h>
#include <string.h>
char *crypt(const char *key, const char *salt) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "%s%s", salt, key);
    return buf;
}
char *getpass(const char *prompt) {
    static char buf[128];
    fprintf(stderr, "%s", prompt);
    if (!fgets(buf, sizeof(buf), stdin)) return "";
    buf[strcspn(buf, "\n")] = 0;
    return buf;
}
]=])
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/dropbear_android_compat.h [=[
#ifndef _DROPBEAR_ANDROID_COMPAT_H
#define _DROPBEAR_ANDROID_COMPAT_H
char *crypt(const char *, const char *);
char *getpass(const char *);
#endif
]=])
    list(APPEND DROPBEAR_CONFIGURE_ARGS
        --host=aarch64-linux-android
        --disable-openpty
        ac_cv_func_crypt=yes
        "CC=${TVM_CC}"
        "LDFLAGS=-pie"
    )
    set(DROPBEAR_MAKE_CFLAGS "-include ${CMAKE_CURRENT_BINARY_DIR}/dropbear_android_compat.h")
    set(DROPBEAR_MAKE_LDFLAGS "LDFLAGS=-pie ${DROPBEAR_BUILD}/dropbear_android_stubs.o")
    # Two android-only localoptions appended to the shared file:
    #  - USE_DEV_PTMX: Bionic PTY allocation goes through /dev/ptmx (no libutil
    #    openpty on the target API in android2's port).
    #  - COMPAT_USER_SHELLS: bionic has no getusershell, so dropbear uses its
    #    bundled compat one, which reads /etc/shells (absent on the android
    #    host) then falls back to this list. TVM's login shell is /tvm/bin/sh,
    #    so it must be here or every login is "invalid shell, rejected".
    set(DROPBEAR_LOCALOPTIONS_CMD sh -c
        "cat ${THIRDPARTY_SRC}/dropbear/localoptions.h > ${DROPBEAR_BUILD}/localoptions.h && printf '#define USE_DEV_PTMX 1\\n#define COMPAT_USER_SHELLS \"/tvm/bin/sh\",\"/bin/sh\"\\n' >> ${DROPBEAR_BUILD}/localoptions.h && ${TVM_CC} -g -c ${CMAKE_CURRENT_BINARY_DIR}/dropbear_android_stubs.c -o ${DROPBEAR_BUILD}/dropbear_android_stubs.o")
endif()

ExternalProject_Add(dropbear_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/dropbear
    BINARY_DIR ${DROPBEAR_BUILD}
    PATCH_COMMAND ${DROPBEAR_LOCALOPTIONS_CMD}
    CONFIGURE_COMMAND ${THIRDPARTY_SRC}/dropbear/configure ${DROPBEAR_CONFIGURE_ARGS}
    BUILD_COMMAND make
        "PROGRAMS=dropbear dbclient dropbearkey scp sftpserver"
        MULTI=1
        "CFLAGS=-g -gdwarf-4 -DDROPBEAR_FORKLESS ${DROPBEAR_MAKE_CFLAGS}"
        ${DROPBEAR_MAKE_LDFLAGS}
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

set(MKSH_LINK_FLAGS "-g -gdwarf-4")
if(ANDROID)
    string(APPEND MKSH_LINK_FLAGS " -pie")
endif()

ExternalProject_Add(mksh_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/mksh
    BINARY_DIR ${MKSH_BUILD}
    CONFIGURE_COMMAND sh -c "cd ${MKSH_BUILD} && \
        CC=${TVM_CC} CFLAGS='-g -gdwarf-4 -fcommon -DMKSH_FORKLESS=1' \
        sh ${THIRDPARTY_SRC}/mksh/Build.sh"
    BUILD_COMMAND sh -c "cd ${MKSH_BUILD} && sh ./Rebuild.sh && \
        ${TVM_CC} ${MKSH_LINK_FLAGS} -o mksh ./*.o"
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
elseif(ANDROID)
    # bionic has no ncurses/termcap — use the same link-time stubs as iOS
    # rather than cross-compiling ncurses; TVM intercepts tputs at runtime.
    list(APPEND VIM_LINK_LIBS ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libtermcap_stubs.a)
    set(VIM_BUILD_ENV ${CMAKE_COMMAND} -E env "CC=${TVM_CC}"
        "TVM_STUBS_DIR=${STUBS_DIR}" "TVM_STUBS_LIB_DIR=${LIB_OUT}"
        "TVM_ANDROID_CROSS=1")
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
if(ANDROID)
    # configure's --with-tlib=termcap_stubs link probe needs the archive first
    ExternalProject_Add_StepDependencies(vim_ext configure termcap_stubs)
endif()
add_custom_target(vim_exe DEPENDS vim_ext)
set_target_properties(vim_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_shared_lib(vim "objects/*.o" ${VIM_LINK_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR ANDROID)
    add_dependencies(vim_shared termcap_stubs)
endif()
add_launcher(vim)
