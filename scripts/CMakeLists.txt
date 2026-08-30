set(ROOTDIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(VIM_RUNTIME ${ROOTDIR}/third_party/vim/runtime)
set(LIB_OUT ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})
set(BIN_OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
set(THIRDPARTY_BUILD ${CMAKE_CURRENT_BINARY_DIR}/../third_party)

# Flutter initrd arch tag — matches build.py platform names (ios-arm64,
# macos-arm64, linux-x64). The initrd holds arch-specific binaries, so each
# platform's app bundles its own via pubspec `platforms:`.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(FLUTTER_ARCH ios-arm64)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(FLUTTER_ARCH macos-arm64)
else()
    set(FLUTTER_ARCH linux-x64)
endif()

# ── Toybox applets ──────────────────────────────────────────────
set(TOYBOX_APPLETS
    ascii base32 base64 basename bzcat cal cat chgrp chmod chown
    cksum clear cmp comm count cp cpio crc32 cut date df dirname
    dos2unix du echo egrep env expand factor fallocate false fgrep
    file find flock fmt fold ftpget ftpput getconf grep groups head
    help hexedit hostname httpd id install kill killall5 link ln
    logger logname ls mcookie md5sum microcom mkdir mkfifo mktemp mv
    nc netcat nice nl nohup od paste patch printenv printf pwd pwdx
    pwgen readelf readlink realpath renice rev rm rmdir sed seq
    setsid sha1sum sha224sum sha256sum sha384sum sha3sum sha512sum
    shred sleep sort split stat strings tac tail tar tee test time
    timeout touch true truncate tty uname unicode uniq unix2dos
    unlink usleep uudecode uuencode uuidgen w watch wc which who
    whoami xargs xxd yes zcat
)

# Dropbear multi-call names
set(DROPBEAR_APPLETS scp dbclient ssh dropbearkey ssh-keygen sshd)

# Vim runtime files to bundle
set(VIM_RUNTIME_FILES
    defaults.vim filetype.vim ftoff.vim ftplugin.vim
    ftplugof.vim indent.vim indoff.vim
)
set(VIM_SYNTAX_FILES
    syntax synload syncolor nosyntax c cpp python sh bash vim
    conf json yaml xml html css javascript markdown make diff help
)

# ── Helper: populate rootfs with shared content ─────────────────
# Creates a shell script that populates the rootfs with symlinks,
# vim runtime, and cpio archive. Called by both initrd targets.
function(mkinitrd_target NAME)
    cmake_parse_arguments(ARG "FLUTTER" "" "DEPENDS" ${ARGN})

    set(ROOTFS ${CMAKE_CURRENT_BINARY_DIR}/.rootfs_${NAME})
    if(ARG_FLUTTER)
        # Per-arch name; pubspec `platforms:` bundles only the matching one.
        set(OUTFILE ${ROOTDIR}/flutter/assets/initrd.flutter.${FLUTTER_ARCH}.cpio)
    else()
        set(OUTFILE ${LIB_OUT}/initrd.cli.cpio)
    endif()

    # Build the shell script inline
    set(SCRIPT "#!/bin/sh\nset -e\n")
    string(APPEND SCRIPT "ROOTFS='${ROOTFS}'\n")
    string(APPEND SCRIPT "rm -rf \"$ROOTFS\" && mkdir -p \"$ROOTFS/bin\"\n\n")

    # ── Binaries ──
    if(ARG_FLUTTER)
        string(APPEND SCRIPT "cp '${BIN_OUT}/mksh_launcher' \"$ROOTFS/bin/sh\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/dropbear_launcher' \"$ROOTFS/bin/dropbear\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/sftpserver_launcher' \"$ROOTFS/bin/sftp-server\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/toybox_launcher' \"$ROOTFS/bin/toybox\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/vim_launcher' \"$ROOTFS/bin/vim\"\n")
    else()
        # Real binaries for CLI
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/dropbear/dropbearmulti' \"$ROOTFS/bin/dropbear\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/sftpserver/sftp-server' \"$ROOTFS/bin/sftp-server\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/mksh/mksh' \"$ROOTFS/bin/sh\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/toybox/toybox' \"$ROOTFS/bin/toybox\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/vim/vim' \"$ROOTFS/bin/vim\"\n")
    endif()
    string(APPEND SCRIPT "ln -sf vim \"$ROOTFS/bin/vi\"\n\n")

    # ── apple *_cmds ──
    foreach(module ${APPLE_CMDS_MODULES})
        foreach(cmd ${APPLE_CMDS_${module}})
            string(APPEND SCRIPT
                "cp '${APPLE_CMDS_BUILD}/${module}/bin/${cmd}' \"$ROOTFS/bin/${cmd}\"\n")
        endforeach()
    endforeach()

    # ── Dropbear symlinks ──
    foreach(applet ${DROPBEAR_APPLETS})
        string(APPEND SCRIPT "ln -sf dropbear \"$ROOTFS/bin/${applet}\"\n")
    endforeach()
    string(APPEND SCRIPT "\n")

    # ── Toybox applet symlinks ──
    string(APPEND SCRIPT "cd \"$ROOTFS\"\n")
    foreach(applet ${TOYBOX_APPLETS})
        string(APPEND SCRIPT "[ ! -e \"bin/${applet}\" ] && ln -sf toybox \"bin/${applet}\"\n")
    endforeach()
    string(APPEND SCRIPT "\n")

    # ── Vim runtime ──
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/share/vim/syntax\"\n")
    foreach(f ${VIM_RUNTIME_FILES})
        string(APPEND SCRIPT "cp '${VIM_RUNTIME}/${f}' \"$ROOTFS/share/vim/\"\n")
    endforeach()
    foreach(f ${VIM_SYNTAX_FILES})
        string(APPEND SCRIPT "[ -f '${VIM_RUNTIME}/syntax/${f}.vim' ] && cp '${VIM_RUNTIME}/syntax/${f}.vim' \"$ROOTFS/share/vim/syntax/\"\n")
    endforeach()
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/share/vim/autoload/dist\"\n")
    string(APPEND SCRIPT "cp '${VIM_RUNTIME}/autoload/dist/ft.vim' \"$ROOTFS/share/vim/autoload/dist/\"\n")
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/share/vim/colors/lists\"\n")
    string(APPEND SCRIPT "cp '${VIM_RUNTIME}/colors/lists/default.vim' \"$ROOTFS/share/vim/colors/lists/\"\n\n")

    # ── Create cpio ──
    # Always pack with our own host toybox's cpio — hermetic, no dependency on a
    # host cpio, and it runs on the build machine for every target (the host
    # toybox is built with the host compiler even when the target is iOS).
    set(CPIO "'${THIRDPARTY_BUILD}/toybox_host/toybox' cpio")
    get_filename_component(OUTDIR "${OUTFILE}" DIRECTORY)
    string(APPEND SCRIPT "mkdir -p '${OUTDIR}'\n")
    string(APPEND SCRIPT "cd \"$ROOTFS\" && find . | ${CPIO} -o -H newc > '${OUTFILE}'\n")

    # Write and register target
    set(SCRIPT_FILE ${CMAKE_CURRENT_BINARY_DIR}/mkinitrd_${NAME}.sh)
    file(GENERATE OUTPUT ${SCRIPT_FILE} CONTENT "${SCRIPT}")

    add_custom_target(${NAME}
        COMMAND sh ${SCRIPT_FILE}
        DEPENDS ${ARG_DEPENDS}
        COMMENT "Assembling ${NAME}"
    )
    set_target_properties(${NAME} PROPERTIES EXCLUDE_FROM_ALL TRUE)
endfunction()

# ── initrd targets ──────────────────────────────────────────────

# The apple *_cmds modules go into both initrds as real executables, so both
# targets depend on them. Empty on Linux, where the subdirectory is not added.
set(APPLE_CMDS_DEPS "")
foreach(module ${APPLE_CMDS_MODULES})
    list(APPEND APPLE_CMDS_DEPS ${module}_exe)
endforeach()

# CLI initrd (real binaries)
mkinitrd_target(initrd_cli
    DEPENDS dropbear_exe sftpserver_ext mksh_exe toybox_exe vim_exe toybox_host_ext ${APPLE_CMDS_DEPS}
)

# Flutter initrd (launcher stubs for the dylib-backed tools, real binaries for
# the apple *_cmds modules)
mkinitrd_target(initrd_flutter FLUTTER
    DEPENDS dropbear_launcher sftpserver_launcher mksh_launcher toybox_launcher vim_launcher toybox_host_ext ${APPLE_CMDS_DEPS}
)
