# DarkSword Post-Exploitation Analysis

Analysis of `DarkSword-RCE/pe_main.js` — the post-exploitation payload that runs after `darksword.c` achieves kernel read/write.

## Overview

pe_main.js is a webpack-bundled JavaScript file (~8440 lines) that implements full iOS post-exploitation. It runs in a browser/WebKit exploit context using `fcall()` to call native functions. After establishing KRW via the same ICMPv6 socket corruption as darksword.c, it:

1. Escapes the sandbox
2. Injects code into system daemons (launchd, SpringBoard, configd, wifid, securityd, UserEventAgent)
3. Exfiltrates sensitive user data via HTTP
4. Cleans up crash reports

It does NOT jailbreak, install persistence, or modify system files. It's a **one-shot forensic extraction / spyware tool**.

## KRW Primitive

Identical to darksword.c: corrupted ICMPv6 socket pair.
- `setsockopt(controlSocket, ICMP6_FILTER, ...)` writes target address to rwSocket's filter pointer
- `getsockopt(rwSocket, ICMP6_FILTER, ...)` reads from that address
- `EARLY_KRW_LENGTH = 0x20` (32 bytes per operation)
- `kread_length()`/`kwrite_length()` loop for larger reads

Two exploit paths: `pe_v1` (non-A18) and `pe_v2` (A18/iPhone 16 — uses wired memory technique with 2GB allocation).

## Post-Exploitation Flow

### Step 1 — Kernel Base Discovery

Unlike darksword.c (which walks `pr_input` → page scan), pe_main.js uses the zone allocator:
```
rw_socket_pcb + 0x20 → control_socket_pcb
control_socket_pcb + 0x38 → pcbinfo_pointer
pcbinfo_pointer + 0x68 → ipi_zone
ipi_zone + 0x10 → zv_name (zone name string, lives in kernel __TEXT)
```
Page-aligns `zv_name` and scans backwards for Mach-O magic (`0xfeedfacf`).

### Step 2 — Find Self Task

- Reads `kernel_base + offsets.kernelTask` → `kernel_task`
- Walks the task linked list (`nextTask`/`prevTask` at +0x30/+0x38)
- For each task: reads `procRO` → dereferences → reads PID
- Matches `getpid()` to find own task

### Step 3 — Sandbox Escape

Walks: `proc_ro` → `ucred` → `cr_label` → sandbox profile → extension set.

Finds the `com.apple.app-sandbox.read-write` extension and zeroes its `data` and `data_length` fields, making it match everything (wildcard). This grants full filesystem access.

### Step 4 — Sandbox Token Creation (via launchd injection)

Injects into **launchd** (PID 1) using EXC_GUARD exception port hijacking:
- Writes `guard_exc_code` and sets `AST_GUARD` flag on target thread
- When thread returns to userland, triggers EXC_GUARD exception
- Attacker handles exception, replies with corrupted `thread_state` (fake PC/LR)
- Creates a "trojan thread" that faults on each call — allows calling arbitrary functions in the target

From launchd, calls `sandbox_extension_issue_file()` for extensive sensitive paths:
- SMS/iMessage databases
- Call history
- Contacts / Address book
- Voicemail
- Safari history and cookies
- Location data / CoreRoutine
- Photos (PhotoData, DCIM)
- Notes, Calendar, Mail
- Health data
- Keychain database + keybag files
- WiFi passwords
- iCloud Drive documents
- Trust store certificates
- Device lockdown info

### Step 5 — System Daemon Injection

Injects JavaScript payloads into multiple daemons:

| Target | Payload | Purpose |
|--------|---------|---------|
| **launchd** (PID 1) | sandbox token issuance | Grants file access tokens |
| **SpringBoard** | `loader.js` | Primary agent + file download |
| **configd** | `keychain_copier.js` | Copies keychain DB + keybags to `/tmp` |
| **wifid** | `wifi_password_dump.js` | Extracts WiFi credentials |
| **securityd** | `wifi_password_securityd.js` | Fallback WiFi credential extraction |
| **UserEventAgent** | `icloud_dumper.js` | iCloud Drive file access |

Injection mechanism (`InjectJS`): finds target process in kernel task list, creates remote JSContext, replaces an NSBlock's invocation pointer to get code execution, shares native buffer for communication.

### Step 6 — Data Exfiltration

After 5-second wait for dumps to complete:
- Injects `file_downloader.js` into SpringBoard
- Exfiltrates all collected data via HTTP
- Sends keychain files from `/tmp`

### Step 7 — Cleanup

- Deletes crash reports from DiagnosticReports directory
- Exits cleanly with `exit(0)`

## iOS 18.4+ MIG Filter Bypass

On iOS 18.4+, a MIG (Mach Interface Generator) sandbox filter blocks exception port manipulation. pe_main.js spawns a separate JS thread that monitors and manipulates MIG filter lock state using `migLock`, `migSbxMsg`, `migKernelStackLR` offsets.

## Kernel Struct Offsets Used

### inpcb (socket PCB)
| Field | Offset | Notes |
|-------|--------|-------|
| `inp_list_next` | `0x28` | Actually `le_prev` |
| `pcbinfo` | `0x38` | |
| `socket_ptr` | `0x40` | `inp_socket` |
| `inp_gencnt` | `0x78` | |
| `icmp6filt` | `0x148` | `inp_depend6.inp6_icmp6filt` |

### socket
| Field | Offset |
|-------|--------|
| `so_count` | `0x254` |

### pcbinfo
| Field | Offset |
|-------|--------|
| `ipi_zone` | `0x68` |

### zone
| Field | Offset |
|-------|--------|
| `zv_name` | `0x10` |

### task
| Field | Offset |
|-------|--------|
| `map` | `0x28` |
| `next` | `0x30` |
| `prev` | `0x38` |
| `threads` | `0x58` |
| `ipc_space` | `0x300` |
| `proc_ro` | `0x3a0` |
| `proc_size` | `0x740` |
| `exc_guard` | `0x5d4` |

### thread
| Field | Offset |
|-------|--------|
| `options` | `0x70` |
| `kstackptr` | `0xf0` |
| `rop_pid` | `0x160` |
| `jop_pid` | `0x168` |
| `guard_exc_code` | `0x330` |
| `task_threads` | `0x370` |
| `tro` | `0x380` |
| `ast` | `0x3a4` |
| `mutex_data` | `0x3b0` |
| `ctid` | `0x430` |

### proc
| Field | Offset |
|-------|--------|
| `pid` | `0x60` |
| `p_comm` | `0x568` |

### proc_ro / ucred / sandbox
| Field | Offset |
|-------|--------|
| `proc_ro → proc_ptr` | `0x0` |
| `proc_ro → ucred` | `0x18` |
| `ucred → cr_label` | `0x78` |
| `cr_label → sandbox_slot` | `0x10` |
| `sandbox → ext_set` | `0x10` |
| `extension → data` | `0x40` |
| `extension → data_length` | `0x48` |

### vm_map / vm_object
| Field | Offset |
|-------|--------|
| `vm_map_hdr → rbh_root` | `0x38` |
| `vm_rbe → left` | `0x0` |
| `vm_rbe → right` | `0x8` |
| `vm_object → vou_size` | `0x18` |
| `vm_object → ref_count` | `0x28` |

### Other
| Field | Offset |
|-------|--------|
| `ipc_entry → object` | `0x0` |
| `ipc_object → kobject` | `0x48` |
| `ipc_port → ip_nsrequest` | `0x58` |
| `ipc_port → ip_sorights` | `0x84` |
| `tro → task` | `0x20` |
| `vm_named_entry → backing_copy` | `0x10` |
| `vm_named_entry → next` | `0x20` |

## Target Versions

**Devices**: iPhone 11 through iPhone 16 (A12–A18)

**iOS versions**: 17.0 through 18.6 (XNU 23.x–24.x)

**T1SZ_BOOT**: 25 for A12–A15, 17 for A16+ (affects PAC bit width)

## Key Differences from darksword.c

| Aspect | darksword.c | pe_main.js |
|--------|-------------|------------|
| Language | C (compiled Mach-O) | JavaScript (browser exploit) |
| KRW primitive | Same ICMPv6 socket corruption | Same |
| Kernel base method | `pr_input` → page scan | `ipi_zone` → `zv_name` → page scan |
| Post-exploit | None (prints "win??") | Full data exfiltration |
| Socket offsets | Hardcoded for one build | Offset table for 100+ builds |
| PAC handling | `XPACI` instruction | Software sign-extension |
| Sandbox escape | None | Wildcard extension zeroing |
| Process injection | None | EXC_GUARD trojan thread |

## Notable: pe_main.js Confirms Our 22B91 Offsets

The offset table in pe_main.js for iPhone13,2 iOS 18.1.1 uses the **same offsets** we derived from IDA:
- `icmp6filt = 0x148` ✓
- `socket_ptr = 0x40` ✓
- `so_count = 0x254` ✓
- `inp_gencnt = 0x78` ✓
- `kernel_base = 0xfffffff007004000` ✓
