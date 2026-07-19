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
        string(APPEND SCRIPT "cp '${BIN_OUT}/toybox_launcher' \"$ROOTFS/bin/toybox\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/vim_launcher' \"$ROOTFS/bin/vim\"\n")
    else()
        # Real binaries for CLI
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/dropbear/dropbearmulti' \"$ROOTFS/bin/dropbear\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/mksh/mksh' \"$ROOTFS/bin/sh\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/toybox/toybox' \"$ROOTFS/bin/toybox\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/vim/vim' \"$ROOTFS/bin/vim\"\n")
    endif()
    string(APPEND SCRIPT "ln -sf vim \"$ROOTFS/bin/vi\"\n\n")

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
    # Pack with our own toybox's cpio so the build needs no host cpio (the CI
    # Linux runner has none). The CLI toybox is built for the host arch, so it
    # runs here. The Flutter initrd keeps the host cpio: its toybox is built for
    # the target (cross-arch on iOS — unrunnable on the macOS build host), and
    # macOS always ships cpio.
    if(ARG_FLUTTER)
        set(CPIO "cpio")
    else()
        set(CPIO "'${THIRDPARTY_BUILD}/toybox/toybox' cpio")
    endif()
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

# CLI initrd (real binaries)
mkinitrd_target(initrd_cli
    DEPENDS dropbear_exe mksh_exe toybox_exe vim_exe
)

# Flutter initrd (launcher stubs)
mkinitrd_target(initrd_flutter FLUTTER
    DEPENDS dropbear_launcher mksh_launcher toybox_launcher vim_launcher
)
