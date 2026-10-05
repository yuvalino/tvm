<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="rsc/logo.jpg" />
  <img alt="tvm" src="rsc/logo.jpg" width="96" height="96" />
</picture>

# tvm

**Bypass security sandbox restrictions without use of vulnerabilities.**

An open-source, userspace framework that allows running unsigned code and multi-process workflows in highly sandboxed environments (iOS).

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/yuvalino/tvm?sort=semver)](https://github.com/yuvalino/tvm/releases)

[Website](https://yuvalino.com) ·
[Research paper](https://yuvalino.com/rage-against-the-sandbox)

<a href="https://x.com/jachashx"><img alt="Follow Yuval on X" src="https://img.shields.io/badge/X-000000?logo=x&amp;logoColor=white" /></a>

</div>

![tvm demo screenshot](rsc/tvm-demo-screenshot.jpg)

## What's TVM ?

Thread-based process virtualization framework. Lets sandboxed applications with no `fork()` permissions run POSIX-compliant and multi-process reliant software by implementing processes as thread groups.

With TVM, `fork()` creates a new thread group (task) that shares resources like file descriptors and signal handlers. Memory ranges are duplicated with pointer fixup to imitate copy-on-write. A dedicated heap (`rmem.c`) and linker (`ulinker.c`) optimize this to be as fast as possible.

Unsigned code execution is supported via the [Unicorn](https://www.unicorn-engine.org/) CPU emulator. When the emulator hits a signed region, it traps and lets the real CPU take over.

## Building

Requirements: CMake 3.16+, Ninja, Python 3, a C compiler.

On macOS, also install: `brew install gnu-sed openssl`

```sh
# Build TVM CLI executable (default)
python3 build.py

# Build TVM flutter app for iOS
python3 build.py -a ios-arm64 -t flutter

# Build tests
python3 build.py -t tests
```

Output goes to `output/<platform>/bin/` (auto-detected: `linux-x64` or `macos-arm64`).

## Running

```sh
# Run a program in TVM
./output/linux-x64/bin/tvm <program> [args...]

# Run with kernel log output
./output/macos-arm64/bin/tvm --verbose <program>

# Custom initrc
./output/linux-x64/bin/tvm -i path/to/init.rc <program>

# Mount a CPIO archive
./output/linux-x64/bin/tvm -v /tvm:path/to/initrd.cpio <program>
```

The CLI launcher sets `HOME` to the directory it is started from, so per-user
files the guest writes (`.bash_history`, `.gitconfig`, `.dropbear/`, rc files)
land there, not in your real home directory.

## Embedding

`libtvm` exposes one entry point, declared in `tvm/tvm.h`. It boots the VM on
the calling thread and execs the program inside it:

```c
#include "tvm.h"

struct tvm_mount initrd = { .path = "/tvm", .data = cpio_bytes, .size = cpio_len };
struct tvm_config cfg = {
    .log_fd  = STDOUT_FILENO,       // -1 disables kernel logging
    .cwd     = "/some/dir",         // NULL = host cwd
    .initrc  = "/tvm/etc/init.rc",  // NULL = none; may live inside a mount
    .mounts  = &initrd,
    .nmounts = 1,
};
char *argv[] = { "sh", "-l", NULL };
tvm_start("sh", argv, envp, &cfg);  // envp NULL = inherit; cfg NULL = all defaults
perror("tvm_start");                // only reached on a pre-boot failure
```

## Flutter app (iOS / macOS)

`flutter/` is a Flutter app that embeds TVM and runs an SSH server (dropbear)
on-device, letting you `ssh` into a full POSIX environment. The TVM core is
built as `libtvm.dylib`; dropbear/bash/toybox/vim/curl/less/nano/sqlite3/git as dylibs; the
rootfs is a per-platform Flutter asset (`initrd.flutter.<arch>.cpio`). The
native glue (`flutter/tvm/tvm_bindings.c`) `dlopen`s libtvm, resolves
`tvm_start` from it (see [Embedding](#embedding)) and calls it on a worker
thread.

```sh
# Build the app (always release). Pick the platform with -a.
python3 build.py -a ios-arm64   -t flutter
python3 build.py -a macos-arm64 -t flutter
```

The build symlinks the app at `output/<arch>/lib/flutter` (→ `Runner.app` on
iOS, `tvm.app` on macOS) — use that path, consistent with the rest of the
`output/<arch>` layout.

### Running on an iOS device

The device must be paired/trusted (over USB once, then usable over the network
tunnel). Use `xcrun devicectl` with the device name:

```sh
xcrun devicectl device install app --device <device-name> output/ios-arm64/lib/flutter
xcrun devicectl device process launch --device <device-name> com.yuvalino.tvm
# then: ssh -p 2222 mobile@<device-name>.local   (default password: alpine)
```

Signing note: the Runner target is pinned to a development team, and
`iOS.entitlements` requests `increased-memory-limit` — that capability must be
enabled on the App ID in the Apple Developer portal, otherwise signing fails.

### Viewing logs on a device

The app writes its startup info and TVM kernel logs to its in-app console **and**
to real `stderr` (fd 2). To see them from your Mac, launch with
`--terminate-existing --console`:

```sh
xcrun devicectl device process launch --terminate-existing --console \
    --device <device-name> com.yuvalino.tvm
```

**`--terminate-existing` is required.** devicectl only connects the app's
stdout/stderr *"if the app is not already running"* — i.e. on a fresh launch.
A plain launch, or `--console` while the app is already up, shows nothing.

### Testing on a device

```sh
# Runs the integration suite against the app's SSH server on the device.
pytest tests/test_integration.py --device <device-name> --backend flutter
```

## Programs

### Builtin

`tvm` - diagnostics tool (registered at fake `/usr/bin/tvm` path):
- `tvm ps` - task list
- `tvm lsof` - file descriptor tables
- `tvm pwd` - working directory
- `tvm env` - environment variables
- `tvm sorunner` - run shared object
- `tvm cpio` - CPIO archive reader
- `tvm mount` - VFS mount
- `tvm shell` - execute command via `system()`

### Third-party (`third_party/`)

| Program | Description |
|---|---|
| [dropbear](https://github.com/mkj/dropbear) | SSH server, client, and `scp` |
| sftp-server | Standalone SFTPv3 server (exec'd by dropbear for sftp/scp subsystem) |
| [bash](https://www.gnu.org/software/bash/) | GNU Bourne-Again Shell 5.3, the login shell and `/tvm/bin/sh` (unpatched upstream, configured via cache variables and `-D` overrides only; see `scripts/build_bash.sh`). Readline's terminal handling through the bundled ncurses and terminfo database. License: GPLv3 |
| [toybox](http://landley.net/toybox/) | Unix utilities (`ls`, `cat`, `grep`, `id`, etc.) |
| [vim](https://github.com/vim/vim) | Text editor (unpatched upstream, see `scripts/build_vim.sh`). Terminal handling through the bundled ncurses and terminfo database. License: Vim |
| [curl](https://curl.se/) | HTTP(S) client, TLS via OpenSSL; certificate verification through the OS trust store on macOS/iOS (Apple SecTrust), Mozilla's CA bundle on Linux. License: curl |
| [OpenSSL](https://www.openssl.org/) | TLS/crypto library, statically linked into curl. License: Apache-2.0 |
| [less](https://greenwoodsoftware.com/less/) | Pager (less 710, unpatched upstream; `PAGER=less`, `LESS=-R` by default). Terminal handling through the bundled ncurses and terminfo database. License: GPLv3 / Less License |
| [nano](https://www.nano-editor.org/) | GNU nano 9.2 text editor (unpatched upstream, built from git with the pinned gnulib; `EDITOR=nano` by default unless set). Syntax highlighting from its own definitions via `/tvm/etc/nanorc`. License: GPLv3 |
| [ncurses](https://invisible-island.net/ncurses/) | ncurses 6.6 (wide-char), statically linked into bash, vim, less and nano on every platform; a small terminfo database (xterm, screen, tmux, vt100/vt220, linux, ansi, dumb and a few terminal emulators' entries: `TERMINFO_ENTRIES` in `scripts/CMakeLists.txt`) is compiled from its `terminfo.src` into `/tvm/share/terminfo` (`TERMINFO`); `/tvm/etc/bash.bashrc` maps any other `TERM` to xterm-256color. License: MIT |
| [sqlite3](https://sqlite.org/) | SQLite 3.53.4 command-line shell (unpatched upstream, amalgamation generated by its own configure/make; see `scripts/build_sqlite.sh`). FTS4/FTS5, R*Tree/geopoly, JSON, math functions, dbstat/dbpage/stmt/bytecode virtual tables and zlib (`.archive`) compiled in; no extension loading (`SQLITE_OMIT_LOAD_EXTENSION`, there is no dlopen). Plain POSIX file locking on every platform, emulated between tasks by TVM's `fcntl()`. License: public domain |
| [linenoise](https://github.com/antirez/linenoise) | Line editing and history for the sqlite3 shell (statically compiled in). License: BSD-2-Clause |
| [git](https://git-scm.com/) | Git 2.55.0 (unpatched upstream; build knobs in `third_party/configs/git_config.mak`, see `scripts/build_git.sh`). HTTPS through the static libcurl/OpenSSL of the curl tool (Apple SecTrust on macOS/iOS, no CA file needed); the curl helpers `git-remote-http` (`https`, `ftp`, `ftps` are links to it) and `git-http-fetch` are upstream's separate programs in the exec path `/tvm/libexec/git-core`, together with the shell-script helpers; templates in `/tvm/share/git-core/templates`, system config `/tvm/etc/gitconfig`. Pages through `less` (`PAGER`) and edits commit messages in `nano` (`EDITOR`); hooks and `!` aliases run under `/tvm/bin/sh`. No gettext, expat (WebDAV `http-push`), pcre, Perl/Python/Tcl commands, fsmonitor daemon or ssh transport. Set your identity once with `git config --global user.name "Your Name"` and `git config --global user.email you@example.com` (stored in `$HOME/.gitconfig`: the app's data container in the Flutter app; on the CLI, `$HOME` is the directory `tvm` was launched from, see [Running](#running)). License: GPLv2 |
| [unicorn](https://www.unicorn-engine.org/) | CPU emulator (TCI interpreter mode for iOS) |
| Apple *_cmds | BSD userland from apple-oss-distributions (`ifconfig`, `netstat`, `ps`, `sysctl`, etc.) |

Licensing note: bash, nano and less are GPLv3, git is GPLv2 (unicorn is
already GPLv2); everything else is permissively licensed. Nothing links against
them — each is shipped as a separate program (`/tvm/bin/bash`, `libbash.dylib`
in the apps, and likewise for nano, less and git) — so the GPL covers those
binaries and their sources only. One caveat: git is GPLv2-only, and its image
statically links libcurl and OpenSSL 3 (Apache-2.0) for HTTPS, a combination
the FSF considers incompatible with GPLv2 (Debian builds git against GnuTLS for
this reason); review that before redistributing the git binaries.

The login shell is `-bash`: it reads `/tvm/etc/profile` (a login shell does not
read `$ENV`), which sources `$ENV` (e.g. `ENV=$HOME/.forklessrc` in the app's
rc) so per-user rc files keep working; `/tvm/bin/sh` is bash invoked as `sh`
(POSIX mode, reads `$ENV` when interactive) and is what `system(3)` and
`#!/bin/sh` scripts get.

### Examples (`examples/`)

Small test programs: `fork`, `snake`, `stopit`, `hello_jmpbuf`, `gdb_backtrace`.

Build with `python3 build.py -t examples`.

## initrc

TVM runs an init script during `tvm_start()` before the main program starts. The script path is the `initrc` field of `struct tvm_config` (NULL = none); a relative path resolves against the init task's cwd and may live inside a config mount.

The CLI launcher maps its flags onto that field:

| Launcher flags | Behavior |
|---|---|
| (none) | Use `./init.rc` if it exists, silently skip otherwise |
| `-i <path>` / `--initrc <path>` | Use the given path, fail if not found |
| `-n` / `--no-initrc` | No initrc |

Commands:

```sh
mount <hostpath>:<guestpath>   # Mount a CPIO archive into the VFS
setenv <name>=<value>          # Set environment variable (overwrites)
setenv? <name>=<value>         # Set only if not already set
mkdir <path>                   # Create directory (mkdir -p semantics)
umask <mode>                   # Set per-task umask (octal, e.g. 022)
echo <message>                 # Print to kernel log
```

All arguments support variable expansion (`$VAR` or `${VAR}`).

See [INITRC.md](INITRC.md) for the full specification.

## Tests

```sh
# Unit tests (rmem + tvm)
python3 build.py -t tests
./output/linux-x64/bin/tvm ./output/linux-x64/bin/test_tvm
./output/macos-arm64/bin/test_rmem

# Integration tests (pytest)
pip install pytest pexpect
python3 build.py -t initrd
pytest tests/test_integration.py
```

Test runner flags: `-l` list tests, `-k=TEST` filter, `-v` verbose, `-g` debug with gdb.

## Platforms

- Linux x86-64
- macOS arm64
- iOS arm64 (via the Flutter app)
