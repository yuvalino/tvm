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

Mount a CPIO archive into the TVM virtual filesystem.

```
mount <hostpath>:<guestpath>
```

- `hostpath` — path to CPIO file on the host filesystem (relative to cwd)
- `guestpath` — mount point in TVM's virtual filesystem

Example:
```
mount initrd.cpio:/tvm
mount $HOME/extra.cpio:/opt
```

### bind

Lay a symbolic link over a host path, pointing at another path (typically
one of the VM's own programs).

```
bind <target>:<link>
```

- `target` — absolute path the link points to; a VFS path (`/tvm/...`) or a host path
- `link` — absolute path the link appears at; it may exist on the host (it is hidden) or not

To every path call that goes through TVM's devices — `open`, `execve`,
`stat`, `access`, `readlink`, `chmod`, `chown` and their `*at` forms — the
link behaves like a symlink: it is followed to the target, `lstat()` reports
a link and `readlink()` returns the target. Only the last component of a
path is matched (`/bin/sh`, not `/bin/sh/x`), and calls that do not go
through the devices (`unlink`, `rename`, `realpath`, ...) see the host's
file. Directory listings do not show links. Links may point at other links.

Example: scripts name their interpreter by host path (`#!/bin/sh`,
`#!/usr/bin/env bash`), and the host's are not loadable in TVM:
```
bind /tvm/bin/sh:/bin/sh
bind /tvm/bin/env:/usr/bin/env
```

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
