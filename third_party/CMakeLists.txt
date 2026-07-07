# unicorn (CPU emulator)
set(UNICORN_INTERPRETER ON CACHE BOOL "" FORCE)
set(UNICORN_ARCH "x86;aarch64" CACHE STRING "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(unicorn EXCLUDE_FROM_ALL)

set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../scripts)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(IOS_CC "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/ios-cc.sh")
    set(CROSS_CC "${IOS_CC}")
    set(CROSS_AR "ar")
else()
    set(CROSS_CC "cc")
    set(CROSS_AR "ar")
endif()

# Helper: define exe/static/shared targets for a third-party tool.
#   NAME      — tool name
#   SCRIPT    — build script (configure + compile + link exe)
#   OBJGLOB   — glob for .o files relative to SRCDIR
#   ARGN      — extra libraries for linking static/shared
function(add_thirdparty NAME SCRIPT OBJGLOB)
    set(SRCDIR ${CMAKE_CURRENT_SOURCE_DIR}/${NAME})
    set(LIBS "${ARGN}")
    string(REPLACE ";" " " LIBS_STR "${LIBS}")

    # exe: runs the full build script (configure + compile + link)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        add_custom_target(${NAME}_exe
            COMMAND sh ${SCRIPTS_DIR}/${SCRIPT} ${SRCDIR}
            COMMENT "Building ${NAME}"
        )
        set_target_properties(${NAME}_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)
    else()
        # iOS: run build script with cross-compiler wrapper
        add_custom_target(${NAME}_exe
            COMMAND env CC=${IOS_CC} TVM_IOS_CROSS=1
                sh ${SCRIPTS_DIR}/${SCRIPT} ${SRCDIR}
            COMMENT "Building ${NAME} (iOS)"
        )
        set_target_properties(${NAME}_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()

    # static lib
    add_custom_target(${NAME}_static
        COMMAND sh -c "${CROSS_AR} rcs ${SRCDIR}/lib${NAME}.a ${SRCDIR}/${OBJGLOB}"
        DEPENDS ${NAME}_exe
        COMMENT "Archiving ${NAME} (static)"
    )
    set_target_properties(${NAME}_static PROPERTIES EXCLUDE_FROM_ALL TRUE)

    # shared lib
    add_custom_target(${NAME}_shared
        COMMAND sh -c "${CROSS_CC} -dynamiclib -fPIC -g -o ${SRCDIR}/lib${NAME}.dylib ${SRCDIR}/${OBJGLOB} ${LIBS_STR} -install_name lib${NAME}.dylib"
        DEPENDS ${NAME}_exe
        COMMENT "Linking ${NAME} (shared)"
    )
    set_target_properties(${NAME}_shared PROPERTIES EXCLUDE_FROM_ALL TRUE)
endfunction()

# dropbear (SSH)
add_thirdparty(dropbear build_dropbear.sh "obj/*.o"
    ${CMAKE_CURRENT_SOURCE_DIR}/dropbear/libtomcrypt/libtomcrypt.a
    ${CMAKE_CURRENT_SOURCE_DIR}/dropbear/libtommath/libtommath.a
    -lz
)

# mksh (shell)
add_thirdparty(mksh build_mksh.sh "*.o")

# toybox (unix utilities)
set(TOYBOX_LINK_LIBS -lm -lz)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # iOS: no openssl/crypt/util/resolv
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(COMMAND brew --prefix openssl OUTPUT_VARIABLE OPENSSL_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT OPENSSL_PREFIX)
        set(OPENSSL_PREFIX /opt/homebrew/opt/openssl)
    endif()
    list(APPEND TOYBOX_LINK_LIBS -lcrypto -lresolv -lssl -L${OPENSSL_PREFIX}/lib)
else()
    list(APPEND TOYBOX_LINK_LIBS -lcrypto -lresolv -lssl -lcrypt -lutil)
endif()
add_thirdparty(toybox build_toybox.sh "generated/obj/*.o" ${TOYBOX_LINK_LIBS})

# vim (text editor)
set(VIM_LINK_LIBS -lm)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND VIM_LINK_LIBS -lncurses)
endif()
add_thirdparty(vim build_vim.sh "src/objects/*.o" ${VIM_LINK_LIBS})
