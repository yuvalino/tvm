# TVM initrc specification

## Overview

TVM initrc is a simple line-based configuration language for TVM initialization. It is executed during `tvm_start()` before the main program runs.

## Loading behavior

The script path is the `initrc` field of `struct tvm_config` passed to `tvm_start()`; NULL means no initrc. A relative path resolves against the init task's cwd (`cfg->cwd`, or the host cwd) and may point inside one of the config mounts, which are mounted first. `tvm_start()` verifies the file exists before booting and returns -1 with `ENOENT` otherwise.

The CLI launcher maps its flags onto that field:

| Launcher flags | Behavior |
|---|---|
| (none) | Use `./init.rc` if it exists in cwd, silently skip otherwise |
| `-i <path>` / `--initrc <path>` | Use the given path, fail if not found |
| `-n` / `--no-initrc` | No initrc |

## Syntax

- One command per line
- Lines starting with `#` are comments
- Empty lines are ignored
- Leading whitespace is stripped
- Command name is the first whitespace-delimited token
- Remaining text after command name is the argument

## Variable expansion

All argument values support environment variable expansion:

- `$VAR` — expands to the value of environment variable `VAR`
- `${VAR}` — same, with explicit delimiters
- Undefined variables expand to empty string

Variable names consist of alphanumeric characters and underscores.

## Commands

### mount

Mount a CPIO archive, or bind a path over another, in the TVM virtual
filesystem, with mount(8)'s spelling:

```
mount [-t cpio] <hostpath>:<guestpath>
mount --bind|-B|-o bind <source>:<mountpoint>
```

- `hostpath` — path to a CPIO (newc) file on the host filesystem (relative to cwd)
- `guestpath`, `mountpoint` — absolute path in TVM's virtual filesystem; it
  is created if missing, parents included, and need not exist on the host
- `source` — absolute guest path the mount point shows: a file or a directory,
  in a cpio mount, on the host, or under another bind; it need not exist yet
  (a dangling bind is `ENOENT` until it does)

The VFS is one tree under the host's root: a mount is attached where it was
mounted, a later mount at the same path replaces it, and a path that meets no
mount is the host's. Directory listings merge: a mounted directory lists its
own entries and the mounts under it, then the entries the host (or the bind's
source) has of the same path, an own entry covering a host one of the same
name. A cpio mount is read-only (`EROFS`).

A bind mount is the object its source names, links resolved, as `mount
--bind` on Linux: `lstat` of a file bind is the file, `readlink` of a mount
point fails, `realpath` reports the mount point. The mount point itself, and
the directories the VM made to hold mount points, cannot be removed or
renamed (`EBUSY`); a rename or hard link across a mount boundary is `EXDEV`
(`mv` copies instead). Everything under a directory bind of a host directory
goes to that directory. A mount under a bind (`mount --bind X:/mnt` then
`mount --bind Y:/mnt/sub`) shadows the source's entry of that name. A
relative host symlink inside a bound directory that climbs above the mount
point resolves where the host's would, not through the mount point's parent
(a known deviation).

Example: scripts and programs name their interpreter by host path
(`#!/bin/sh`, `#!/usr/bin/env bash`, `system(3)`), and the host's are not
loadable in TVM, so both rc files mount the VM's own over them:
```
mount initrd.cpio:/tvm
mount $HOME/extra.cpio:/opt
mount --bind /tvm/bin/sh:/bin/sh
mount --bind /tvm/bin/env:/usr/bin/env
```

The guest-side builtin takes the same options with two operands:
`tvm mount [-t cpio] FILE TARGET`, `tvm mount --bind SOURCE TARGET`.

### setenv

Set an environment variable (always overwrites).

```
setenv <name>=<value>
```

### setenv?

Set an environment variable only if not already set.

```
setenv? <name>=<value>
```

Example:
```
setenv PATH=/tvm/bin:$PATH
setenv TVM_SHELL=/tvm/bin/sh
setenv? TVM_NOEMU=/tvm/bin/dropbear:/tvm/bin/sh
```

### echo

Print a message to kernel log output.

```
echo <message>
```

Example:
```
echo TVM initialized at $HOME
```

## Example init.rc

```
# Mount rootfs
mount initrd.cpio:/tvm

# Environment setup
setenv TVM_SHELL=/tvm/bin/sh
setenv PATH=/tvm/bin:$PATH
setenv? TVM_NOEMU=/tvm/bin/dropbear:/tvm/bin/sh:/tvm/bin/toybox:/tvm/bin/vim

echo TVM initialized
```
