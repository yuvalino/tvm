# unicorn (CPU emulator)
set(UNICORN_INTERPRETER ON CACHE BOOL "" FORCE)
set(UNICORN_ARCH "x86;aarch64" CACHE STRING "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(unicorn EXCLUDE_FROM_ALL)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../scripts)

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
    add_custom_target(${NAME}_exe
        COMMAND sh ${SCRIPTS_DIR}/${SCRIPT} ${SRCDIR}
        COMMENT "Building ${NAME}"
    )
    set_target_properties(${NAME}_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)

    # static lib
    add_custom_target(${NAME}_static
        COMMAND sh -c "ar rcs ${SRCDIR}/lib${NAME}.a ${SRCDIR}/${OBJGLOB}"
        DEPENDS ${NAME}_exe
        COMMENT "Archiving ${NAME} (static)"
    )
    set_target_properties(${NAME}_static PROPERTIES EXCLUDE_FROM_ALL TRUE)

    # shared lib
    add_custom_target(${NAME}_shared
        COMMAND sh -c "cc -dynamiclib -fPIC -g -o ${SRCDIR}/lib${NAME}.dylib ${SRCDIR}/${OBJGLOB} ${LIBS_STR} -install_name lib${NAME}.dylib"
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
set(TOYBOX_LINK_LIBS -lm -lz -lcrypto -lresolv -lssl)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(COMMAND brew --prefix openssl OUTPUT_VARIABLE OPENSSL_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT OPENSSL_PREFIX)
        set(OPENSSL_PREFIX /opt/homebrew/opt/openssl)
    endif()
    list(APPEND TOYBOX_LINK_LIBS -L${OPENSSL_PREFIX}/lib)
else()
    list(APPEND TOYBOX_LINK_LIBS -lcrypt -lutil)
endif()
add_thirdparty(toybox build_toybox.sh "generated/obj/*.o" ${TOYBOX_LINK_LIBS})

# vim (text editor)
add_thirdparty(vim build_vim.sh "src/objects/*.o" -lm -lncurses)
