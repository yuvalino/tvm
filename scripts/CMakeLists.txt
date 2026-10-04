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
    ascii awk base32 base64 basename bzcat cal cat chgrp chmod chown
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
        # The apps load each tool as lib<tool>.dylib from the bundle's
        # Frameworks directory, which is outside the initrd. /tvm/bin/<tool>
        # is an image reference: an executable text file naming the image by
        # its install name, which TVM's execve() resolves through TVM_RPATH
        # (set by the app bindings to the Frameworks directory) and runs with
        # the caller's argv. See tvm_resolve_image_ref() in tvm/tvm.c.
        foreach(pair "bash:bash" "dropbear:dropbear" "sftpserver:sftp-server" "toybox:toybox"
                     "vim:vim" "curl:curl" "less:less" "nano:nano" "sqlite:sqlite3" "git:git")
            string(REPLACE ":" ";" pair "${pair}")
            list(GET pair 0 lib)
            list(GET pair 1 name)
            string(APPEND SCRIPT "printf '@rpath/lib${lib}.dylib\\n' > \"$ROOTFS/bin/${name}\" && chmod 755 \"$ROOTFS/bin/${name}\"\n")
        endforeach()
    else()
        # Real binaries for CLI
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/dropbear/dropbearmulti' \"$ROOTFS/bin/dropbear\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/sftpserver/sftp-server' \"$ROOTFS/bin/sftp-server\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/bash/bash' \"$ROOTFS/bin/bash\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/toybox/toybox' \"$ROOTFS/bin/toybox\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/vim/vim' \"$ROOTFS/bin/vim\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/curl/src/curl' \"$ROOTFS/bin/curl\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/less/src/less' \"$ROOTFS/bin/less\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/nano/build/src/nano' \"$ROOTFS/bin/nano\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/sqlite/sqlite3' \"$ROOTFS/bin/sqlite3\"\n")
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/git/git' \"$ROOTFS/bin/git\"\n")
    endif()
    string(APPEND SCRIPT "ln -sf vim \"$ROOTFS/bin/vi\"\n")
    # /bin/sh is bash invoked as `sh`: POSIX mode, reads $ENV when interactive
    # (scripts, system(3) and dropbear's `sh -c` all go through it).
    string(APPEND SCRIPT "ln -sf bash \"$ROOTFS/bin/sh\"\n\n")

    # ── Shell startup files ──
    # bash reads SYS_PROFILE (=/tvm/etc/profile, see scripts/build_bash.sh) for
    # login shells and SYS_BASHRC (=/tvm/etc/bash.bashrc) for interactive
    # non-login shells. Missing files are skipped silently.
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/etc\"\n")
    string(APPEND SCRIPT "cp '${ROOTDIR}/third_party/configs/bash_profile' \"$ROOTFS/etc/profile\"\n")
    string(APPEND SCRIPT "cp '${ROOTDIR}/third_party/configs/bash_bashrc' \"$ROOTFS/etc/bash.bashrc\"\n\n")

    # ── Terminfo database ──
    # bash (readline), vim, less and nano link one static ncursesw whose
    # compiled-in database is /tvm/share/terminfo (the rc files export
    # TERMINFO to the same path), so this is the only terminal description
    # in the VM. Compiled here from ncurses' own terminfo.src with the
    # host-built tic (ncurses_host_ext), limited to the terminals ssh
    # clients commonly announce (a name missing from terminfo.src is skipped
    # by tic); /tvm/etc/bash.bashrc maps anything else to xterm-256color.
    set(TERMINFO_ENTRIES
        xterm xterm-256color screen screen-256color tmux tmux-256color
        vt100 vt220 linux ansi dumb
        alacritty wezterm foot konsole-256color gnome-256color putty-256color)
    string(REPLACE ";" "," TERMINFO_ENTRIES_CSV "${TERMINFO_ENTRIES}")
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/share/terminfo\"\n")
    string(APPEND SCRIPT "'${THIRDPARTY_BUILD}/ncurses_host/progs/tic' -x -e ${TERMINFO_ENTRIES_CSV} -o \"$ROOTFS/share/terminfo\" '${ROOTDIR}/third_party/ncurses/misc/terminfo.src'\n\n")

    # ── nano ──
    # System nanorc (SYSCONFDIR=/tvm/etc, see scripts/build_nano.sh) and the
    # syntax definitions it includes from /tvm/share/nano.
    string(APPEND SCRIPT "cp '${ROOTDIR}/third_party/configs/nanorc' \"$ROOTFS/etc/nanorc\"\n")
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/share/nano\"\n")
    string(APPEND SCRIPT "cp '${ROOTDIR}/third_party/nano/syntax/'*.nanorc \"$ROOTFS/share/nano/\"\n\n")

    # ── git ──
    # The exec path (git --exec-path = /tvm/libexec/git-core, compiled in; see
    # third_party/configs/git_config.mak): everything git execs by name. The
    # curl helpers and upstream's dashed built-in names are links to the one
    # multi-call /tvm/bin/git (third_party/git_main.c) -- absolute, so that
    # TVM_NOEMU's symlink resolution lands on /tvm/bin/git -- and git itself
    # is there too because git prepends the exec path to PATH and runs its
    # subcommands as `git <cmd>`. Next to them the generated shell scripts
    # (#!/tvm/bin/sh), the mergetool definitions they source, and the `git
    # init` templates at the compiled-in template_dir.
    set(GIT_BUILD ${THIRDPARTY_BUILD}/git)
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/libexec/git-core\" \"$ROOTFS/share/git-core\"\n")
    foreach(prog git git-remote-http git-remote-https git-remote-ftp git-remote-ftps git-http-fetch)
        string(APPEND SCRIPT "ln -sf /tvm/bin/git \"$ROOTFS/libexec/git-core/${prog}\"\n")
    endforeach()
    string(APPEND SCRIPT "for n in $(cat '${GIT_BUILD}/tvm/built_ins'); do ln -sf /tvm/bin/git \"$ROOTFS/libexec/git-core/$n\"; done\n")
    string(APPEND SCRIPT "for n in $(cat '${GIT_BUILD}/tvm/scripts'); do cp '${GIT_BUILD}/src/'\"$n\" \"$ROOTFS/libexec/git-core/$n\"; done\n")
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/libexec/git-core/mergetools\"\n")
    string(APPEND SCRIPT "cp '${ROOTDIR}/third_party/git/mergetools/'* \"$ROOTFS/libexec/git-core/mergetools/\"\n")
    string(APPEND SCRIPT "cp -R '${GIT_BUILD}/src/templates/blt' \"$ROOTFS/share/git-core/templates\"\n\n")

    # ── CA bundle (Linux only) ──
    # curl verifies through the OS trust store on macOS/iOS (USE_APPLE_SECTRUST);
    # Linux gets Mozilla's bundle, curl's compile-time CURL_CA_BUNDLE default.
    if(NOT APPLE)
        string(APPEND SCRIPT "mkdir -p \"$ROOTFS/etc/ssl\"\n")
        string(APPEND SCRIPT "cp '${ROOTDIR}/third_party/configs/cacert.pem' \"$ROOTFS/etc/ssl/cacert.pem\"\n\n")
    endif()

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
    DEPENDS dropbear_exe sftpserver_ext bash_exe toybox_exe vim_exe curl_exe less_exe nano_exe sqlite_exe git_exe toybox_host_ext ncurses_host_ext ${APPLE_CMDS_DEPS}
)

# Flutter initrd (image references for the dylib-backed tools, real binaries
# for the apple *_cmds modules; git_exe for the exec-path name lists)
mkinitrd_target(initrd_flutter FLUTTER
    DEPENDS git_exe toybox_host_ext ncurses_host_ext ${APPLE_CMDS_DEPS}
)
