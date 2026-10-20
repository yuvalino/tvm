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

# clang's configuration (third_party/configs/clang.cfg): the deployment target
# is the one scripts/build_llvm.sh builds the runtimes for
if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(TVM_CLANG_MIN_VERSION_FLAG -mios-version-min=15.0)
    else()
        set(TVM_CLANG_MIN_VERSION_FLAG -mmacosx-version-min=13.0)
    endif()
    set(CLANG_CFG ${CMAKE_CURRENT_BINARY_DIR}/clang.cfg)
    configure_file(${ROOTDIR}/third_party/configs/clang.cfg ${CLANG_CFG} @ONLY)
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
set(DROPBEAR_APPLETS scp dbclient ssh dropbearkey ssh-keygen)

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
        string(APPEND SCRIPT "cp '${BIN_OUT}/bash_launcher' \"$ROOTFS/bin/bash\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/dropbear_launcher' \"$ROOTFS/bin/dropbear\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/sftpserver_launcher' \"$ROOTFS/bin/sftp-server\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/toybox_launcher' \"$ROOTFS/bin/toybox\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/vim_launcher' \"$ROOTFS/bin/vim\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/curl_launcher' \"$ROOTFS/bin/curl\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/less_launcher' \"$ROOTFS/bin/less\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/nano_launcher' \"$ROOTFS/bin/nano\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/sqlite_launcher' \"$ROOTFS/bin/sqlite3\"\n")
        string(APPEND SCRIPT "cp '${BIN_OUT}/git_launcher' \"$ROOTFS/bin/git\"\n")
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
        string(APPEND SCRIPT "cp '${THIRDPARTY_BUILD}/git/src/git' \"$ROOTFS/bin/git\"\n")
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
    # third_party/configs/git_config.mak): everything git execs by name, as
    # upstream installs it. The curl helpers git-remote-http (https, ftp and
    # ftps are links to it) and git-http-fetch are programs of their own --
    # real executables in the CLI initrd, launcher stubs for their dylibs in
    # the Flutter one. Upstream's dashed built-in names and git itself are
    # links to /tvm/bin/git -- absolute, so that TVM_NOEMU's symlink
    # resolution lands on /tvm/bin/git; git is there because git prepends the
    # exec path to PATH and runs its subcommands as `git <cmd>`. Next to them
    # the generated shell scripts (#!/tvm/bin/sh), the mergetool definitions
    # they source, and the `git init` templates at the compiled-in
    # template_dir.
    set(GIT_BUILD ${THIRDPARTY_BUILD}/git)
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/libexec/git-core\" \"$ROOTFS/share/git-core\"\n")
    string(APPEND SCRIPT "ln -sf /tvm/bin/git \"$ROOTFS/libexec/git-core/git\"\n")
    foreach(prog git-remote-http git-http-fetch)
        if(ARG_FLUTTER)
            string(APPEND SCRIPT "cp '${BIN_OUT}/${prog}_launcher' \"$ROOTFS/libexec/git-core/${prog}\"\n")
        else()
            string(APPEND SCRIPT "cp '${GIT_BUILD}/src/${prog}' \"$ROOTFS/libexec/git-core/${prog}\"\n")
        endif()
    endforeach()
    foreach(prog git-remote-https git-remote-ftp git-remote-ftps)
        string(APPEND SCRIPT "ln -sf git-remote-http \"$ROOTFS/libexec/git-core/${prog}\"\n")
    endforeach()
    string(APPEND SCRIPT "for n in $(cat '${GIT_BUILD}/tvm/built_ins'); do ln -sf /tvm/bin/git \"$ROOTFS/libexec/git-core/$n\"; done\n")
    string(APPEND SCRIPT "for n in $(cat '${GIT_BUILD}/tvm/scripts'); do cp '${GIT_BUILD}/src/'\"$n\" \"$ROOTFS/libexec/git-core/$n\"; done\n")
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/libexec/git-core/mergetools\"\n")
    string(APPEND SCRIPT "cp '${ROOTDIR}/third_party/git/mergetools/'* \"$ROOTFS/libexec/git-core/mergetools/\"\n")
    string(APPEND SCRIPT "cp -R '${GIT_BUILD}/src/templates/blt' \"$ROOTFS/share/git-core/templates\"\n\n")

    # ── CA bundle ──
    # Mozilla's bundle: curl's compile-time CURL_CA_BUNDLE default on Linux
    # (on macOS/iOS curl verifies through the OS trust store,
    # USE_APPLE_SECTRUST), and SSL_CERT_FILE (init.rc, init.flutter.rc) for
    # programs on OpenSSL's own verification, Python's ssl among them: its
    # compiled-in default is the build tree's OpenSSL prefix.
    string(APPEND SCRIPT "mkdir -p \"$ROOTFS/etc/ssl\"\n")
    string(APPEND SCRIPT "cp '${ROOTDIR}/third_party/configs/cacert.pem' \"$ROOTFS/etc/ssl/cacert.pem\"\n\n")

    # ── security ──
    # The keychain tool, as an empty keychain (scripts/security.sh): the
    # host's cannot run in TVM, and Claude Code saves its login only when the
    # keychain answers like one
    if(APPLE)
        string(APPEND SCRIPT "cp '${ROOTDIR}/scripts/security.sh' \"$ROOTFS/bin/security\"\n")
        string(APPEND SCRIPT "chmod 755 \"$ROOTFS/bin/security\"\n\n")
    endif()

    # ── python ──
    # bin/python3.X is the interpreter: a launcher stub for libpython in the
    # Flutter initrd, the real executable in the CLI one; python3 and python
    # link to it (TVM_NOEMU resolves the links to /tvm/bin/python3.X), and
    # pip, pip3 and pip3.X are pip's console script. The standard library is
    # /tvm/lib/python3.X, the interpreter's compiled-in prefix: carried by
    # the CLI initrd; the apps ship it in the bundle and bind it there
    # (init.flutter.rc), since an initrd is read into memory whole.
    if(APPLE)
        set(PYV ${TVM_PYTHON_VERSION})
        set(PYTHON_STAGE ${THIRDPARTY_BUILD}/python/stage/tvm)
        if(ARG_FLUTTER)
            string(APPEND SCRIPT "cp '${BIN_OUT}/python_launcher' \"$ROOTFS/bin/python${PYV}\"\n")
        else()
            string(APPEND SCRIPT "cp '${PYTHON_STAGE}/bin/python${PYV}' \"$ROOTFS/bin/python${PYV}\"\n")
            string(APPEND SCRIPT "mkdir -p \"$ROOTFS/lib\"\n")
            string(APPEND SCRIPT "cp -R '${PYTHON_STAGE}/lib/python${PYV}' \"$ROOTFS/lib/\"\n")
        endif()
        string(APPEND SCRIPT "ln -sf python${PYV} \"$ROOTFS/bin/python3\"\n")
        string(APPEND SCRIPT "ln -sf python3 \"$ROOTFS/bin/python\"\n")
        foreach(pip pip pip3 pip${PYV})
            string(APPEND SCRIPT "cp '${ROOTDIR}/third_party/configs/pip' \"$ROOTFS/bin/${pip}\"\n")
            string(APPEND SCRIPT "chmod 755 \"$ROOTFS/bin/${pip}\"\n")
        endforeach()
        string(APPEND SCRIPT "\n")
    endif()

    # ── clang ──
    # bin/llvm is the toolchain (scripts/build_llvm.sh): a launcher stub for
    # libllvm in the Flutter initrd; in the CLI one, init.rc binds the
    # executable from the build tree there, as an initrd is read into memory
    # whole. The tools are names of it, which it tells apart by argv[0]
    # (links, which TVM_NOEMU resolves to /tvm/bin/llvm); cc, c++ and cpp,
    # names it does not know, are scripts. The configuration in /tvm/etc/clang
    # points clang at the bundle init.rc binds at /tvm/llvm.
    if(APPLE)
        if(ARG_FLUTTER)
            string(APPEND SCRIPT "cp '${BIN_OUT}/llvm_launcher' \"$ROOTFS/bin/llvm\"\n")
        endif()
        foreach(tool clang clang++ clang-cpp ld64.lld ld ar ranlib nm objdump otool strip size c++filt addr2line)
            string(APPEND SCRIPT "ln -sf llvm \"$ROOTFS/bin/${tool}\"\n")
        endforeach()
        foreach(pair cc:clang c++:clang++ cpp:clang-cpp)
            string(REPLACE ":" ";" pair "${pair}")
            list(GET pair 0 name)
            list(GET pair 1 tool)
            string(APPEND SCRIPT "printf '#!/tvm/bin/sh\\nexec ${tool} \"$@\"\\n' > \"$ROOTFS/bin/${name}\"\n")
            string(APPEND SCRIPT "chmod 755 \"$ROOTFS/bin/${name}\"\n")
        endforeach()
        string(APPEND SCRIPT "mkdir -p \"$ROOTFS/etc/clang\"\n")
        foreach(cfg clang clang++ clang-cpp)
            string(APPEND SCRIPT "cp '${CLANG_CFG}' \"$ROOTFS/etc/clang/${cfg}.cfg\"\n")
        endforeach()
        string(APPEND SCRIPT "\n")
    endif()

    # ── ripgrep ── (TVM_RIPGREP, third_party/CMakeLists.txt): likewise
    if(APPLE AND TVM_RIPGREP AND ARG_FLUTTER)
        string(APPEND SCRIPT "cp '${BIN_OUT}/rg_launcher' \"$ROOTFS/bin/rg\"\n\n")
    endif()

    # ── Node.js ── (TVM_NODE, third_party/CMakeLists.txt): a launcher stub
    # for libnode, and npm and npx run from the modules init.flutter.rc binds
    # at /tvm/node
    if(APPLE AND TVM_NODE AND ARG_FLUTTER)
        string(APPEND SCRIPT "cp '${BIN_OUT}/node_launcher' \"$ROOTFS/bin/node\"\n")
        foreach(pair npm:npm-cli npx:npx-cli)
            string(REPLACE ":" ";" pair "${pair}")
            list(GET pair 0 name)
            list(GET pair 1 cli)
            string(APPEND SCRIPT "printf '#!/tvm/bin/sh\\nexec /tvm/bin/node /tvm/node/npm/bin/${cli}.js \"$@\"\\n' > \"$ROOTFS/bin/${name}\"\n")
            string(APPEND SCRIPT "chmod 755 \"$ROOTFS/bin/${name}\"\n")
        endforeach()
        string(APPEND SCRIPT "\n")
    endif()

    # ── Bun ── (TVM_BUN, third_party/CMakeLists.txt): a launcher stub for
    # libbun in the Flutter initrd, the signed runtime execve runs compiled
    # Bun programs on (tvm.c, "Bun programs"), and what it preloads into
    # them: tvm-sab.js (a SharedArrayBuffer for JavaScriptCore without its
    # JIT) and tvm-cellseg.js (Bun.ant.CellSegmenter, which Claude Code's
    # terminal UI paints through).
    if(APPLE AND TVM_BUN AND ARG_FLUTTER)
        string(APPEND SCRIPT "cp '${BIN_OUT}/bun_launcher' \"$ROOTFS/bin/bun\"\n")
        string(APPEND SCRIPT "mkdir -p \"$ROOTFS/share/bun\"\n")
        string(APPEND SCRIPT "cp '${ROOTDIR}/scripts/bun/tvm-sab.js' \"$ROOTFS/share/bun/tvm-sab.js\"\n")
        string(APPEND SCRIPT "cp '${ROOTDIR}/scripts/bun/tvm-cellseg.js' \"$ROOTFS/share/bun/tvm-cellseg.js\"\n\n")
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
# python: macOS / iOS only (third_party/CMakeLists.txt)
set(PYTHON_CLI_DEPS "")
set(PYTHON_FLUTTER_DEPS "")
if(APPLE)
    set(PYTHON_CLI_DEPS python_exe)
    set(PYTHON_FLUTTER_DEPS python_launcher)
endif()
# clang: macOS / iOS only (third_party/CMakeLists.txt). The CLI's init.rc
# binds the program and the bundle from the build output.
set(LLVM_CLI_DEPS "")
set(LLVM_FLUTTER_DEPS "")
if(APPLE)
    set(LLVM_CLI_DEPS llvm_exe llvm_bundle)
    set(LLVM_FLUTTER_DEPS llvm_launcher)
    if(TVM_RIPGREP)
        list(APPEND LLVM_FLUTTER_DEPS rg_launcher)
    endif()
    if(TVM_NODE)
        list(APPEND LLVM_FLUTTER_DEPS node_launcher)
    endif()
    if(TVM_BUN)
        list(APPEND LLVM_FLUTTER_DEPS bun_launcher)
    endif()
endif()
foreach(module ${APPLE_CMDS_MODULES})
    list(APPEND APPLE_CMDS_DEPS ${module}_exe)
endforeach()

# CLI initrd (real binaries)
mkinitrd_target(initrd_cli
    DEPENDS dropbear_exe sftpserver_ext bash_exe toybox_exe vim_exe curl_exe less_exe nano_exe sqlite_exe git_exe toybox_host_ext ncurses_host_ext ${APPLE_CMDS_DEPS} ${PYTHON_CLI_DEPS} ${LLVM_CLI_DEPS}
)

# Flutter initrd (launcher stubs for the dylib-backed tools, real binaries for
# the apple *_cmds modules)
mkinitrd_target(initrd_flutter FLUTTER
    DEPENDS dropbear_launcher sftpserver_launcher bash_launcher toybox_launcher vim_launcher curl_launcher less_launcher nano_launcher sqlite_launcher git_launcher git-remote-http_launcher git-http-fetch_launcher git_exe toybox_host_ext ncurses_host_ext ${APPLE_CMDS_DEPS} ${PYTHON_FLUTTER_DEPS} ${LLVM_FLUTTER_DEPS}
)
