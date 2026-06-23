# TVM - Thread Virtual Machine

![TembleOS](./rsc/logo.jpg)

Thread-based process virtualization framework. Lets sandboxed applications with no `fork()` permissions run POSIX-compliant programs by implementing processes as thread groups.

`fork()` creates a new thread group (task) that shares resources like file descriptors and signal handlers. Memory ranges are duplicated with pointer fixup to imitate copy-on-write. A dedicated heap (`rmem.c`) and linker (`ulinker.c`) optimize this to be as fast as possible.

Unsigned code execution is supported via the [Unicorn](https://www.unicorn-engine.org/) CPU emulator. When the emulator hits a signed region, it traps and lets the real CPU take over. The `TVM_NOEMU` environment variable controls which programs skip emulation.

## Building

Requirements: CMake 3.16+, Ninja, Python 3, a C compiler.

On macOS, also install: `brew install gnu-sed openssl`

```sh
# Build TVM executable (default)
python3 build.py

# Build specific targets
python3 build.py -t dropbear mksh toybox vim

# Build the initrd (rootfs CPIO archive with all third-party programs)
python3 build.py -t initrd

# Build tests
python3 build.py -t tests

# Build examples
python3 build.py -t examples

# Clean and rebuild
python3 build.py --clean
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

# Skip initrc
./output/macos-arm64/bin/tvm -n <program>

# Mount a CPIO archive
./output/linux-x64/bin/tvm -v /tvm:path/to/initrd.cpio <program>
```

## Programs

### Builtin

`tvm` - diagnostics tool (registered at `/usr/bin/tvm`):
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
| [dropbear](https://github.com/mkj/dropbear) | SSH server and `scp` |
| [mksh](https://github.com/MirBSD/mksh) | MirBSD Korn Shell |
| [toybox](http://landley.net/toybox/) | Unix utilities (`ls`, `cat`, `grep`, `id`, etc.) |
| [vim](https://github.com/vim/vim) | Text editor |

### Examples (`examples/`)

Small test programs: `fork`, `snake`, `stopit`, `hello_jmpbuf`, `gdb_backtrace`.

Build with `python3 build.py -t examples`.

## initrc

TVM runs an init script during `tvm_init()` before the main program starts. By default it looks for `./init.rc` in the current directory.

| Environment variable | Behavior |
|---|---|
| (neither set) | Try `./init.rc`, silently skip if not found |
| `TVM_INITRC=<path>` | Load from given path, error if not found |
| `TVM_NOINITRC=1` | Skip initrc entirely |

Commands:

```sh
mount <hostpath>:<guestpath>   # Mount a CPIO archive into the VFS
setenv <name>=<value>          # Set environment variable (overwrites)
setenv? <name>=<value>         # Set only if not already set
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
