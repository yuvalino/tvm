include(ExternalProject)

# unicorn (CPU emulator) — uses cmake natively
set(UNICORN_INTERPRETER ON CACHE BOOL "" FORCE)
set(UNICORN_ARCH "x86;aarch64" CACHE STRING "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(unicorn EXCLUDE_FROM_ALL)

set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../scripts)
set(THIRDPARTY_SRC ${CMAKE_CURRENT_SOURCE_DIR})
set(THIRDPARTY_BUILD ${CMAKE_CURRENT_BINARY_DIR})
set(LIB_OUT ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})
set(BIN_OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(TVM_CC "${SCRIPTS_DIR}/ios-cc.sh")
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
    CONFIGURE_COMMAND ""
    BUILD_COMMAND sh ${SCRIPTS_DIR}/build_mksh.sh ${THIRDPARTY_SRC}/mksh ${MKSH_BUILD}
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
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=${TVM_CC}" sh ${SCRIPTS_DIR}/build_toybox.sh ${THIRDPARTY_SRC}/toybox ${TOYBOX_BUILD}
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)
add_custom_target(toybox_exe DEPENDS toybox_ext)
set_target_properties(toybox_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_shared_lib(toybox "generated/obj/*.o" ${TOYBOX_LINK_LIBS})
add_launcher(toybox)

###############################################################################
# vim
###############################################################################
set(VIM_BUILD ${THIRDPARTY_BUILD}/vim)

set(VIM_LINK_LIBS -lm)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND VIM_LINK_LIBS ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libtermcap_stubs.a)
else()
    list(APPEND VIM_LINK_LIBS -lncurses)
endif()

ExternalProject_Add(vim_ext
    SOURCE_DIR ${THIRDPARTY_SRC}/vim
    BINARY_DIR ${VIM_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND sh ${SCRIPTS_DIR}/build_vim.sh ${THIRDPARTY_SRC}/vim ${VIM_BUILD}
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
