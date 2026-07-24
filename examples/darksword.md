# DarkSword Exploit — Technical Deep Dive

## The Vulnerability

The vulnerability is a **TOCTOU (Time-of-Check-Time-of-Use) race condition** between userspace file I/O (`pwritev`/`preadv`) and Mach VM remapping (`mach_vm_map` with `VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE`).

When userspace initiates a file I/O operation (e.g., `pwritev`), the kernel resolves the userspace buffer's virtual address to physical pages and pins them for the duration of the I/O. If another thread remaps that same virtual address range to a DIFFERENT physical page (via `mach_vm_map(VM_FLAGS_OVERWRITE)`) while the I/O is in flight, the kernel's I/O operation may read from or write to the **new** physical page instead of the original.

This gives userspace the ability to read/write arbitrary physical pages — including pages belonging to kernel zone allocations (socket PCBs, credentials, etc.).

### The Race

```
Thread A (main)                     Thread B (free_thread)
───────────────────                 ───────────────────────
pcAddress → IOSurface phys page     
                                    
pwritev(fd, {pcAddress+0x3f00},     
        1, 0x3f00)                  
  kernel pins pages at pcAddress    
  kernel starts DMA/copy from       mach_vm_map(pcAddress,
    pcAddress+0x3f00 to file           VM_FLAGS_FIXED|OVERWRITE,
                                       targetObject, offset)
                                     → pcAddress now maps to
                                       TARGET physical page
  kernel continues copy but now     
    reads from TARGET phys page     
  kernel writes target's data       
    into the file                   
                                    
pwritev returns -1 (mapping         
  changed mid-flight)              
                                    
lseek + read from file              
  → file now contains OOB           
    physical data from target page  
```

### Physical Contiguous Mapping (IOSurface trick)

To make the race work reliably, the exploit needs a userspace mapping backed by **physically contiguous** memory. It creates an IOSurface with `IOSurfaceMemoryRegion = "PurpleGfxMem"` (GPU memory region), which is physically contiguous. Then:

1. `IOSurfaceGetBaseAddress()` → gets userspace VA of the surface
2. `mach_make_memory_entry_64()` → creates a Mach memory entry (portable handle to the physical pages)
3. `mach_vm_map()` → maps the memory entry to a new VA (`pcAddress`)

The `pcAddress` mapping can be repeatedly torn down and re-established with different backing objects, which is what enables the race.

## Kernel Heap Spray

### Socket Spray

The exploit sprays ~22,528 ICMPv6 sockets:

```c
socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6)
```

Each socket creates:
- An `inpcb` (protocol control block) — 1024 bytes, allocated from `kalloc.type.1024` zone
- A `struct icmp6_filter` — 32 bytes, allocated from a kalloc.32 zone via `kalloc_type` (in `icmp6_dgram_attach`)

The filter pointer is stored at `inpcb + 0x148` (`in6p_icmp6filt`).

The sockets are converted to Mach fileports (`fileport_makeport`) and the file descriptors are closed. The fileports hold references to keep the sockets alive. During spray, `proc_info` syscall (336) is used to collect each socket's `inp_gencnt` for later identification.

### Memory Layout Goal

The spray fills physical pages with socket PCBs. The search mappings (`mach_vm_allocate`, 8 × 128MB) create userspace mappings that share physical pages with the kernel's zone allocator. When a search mapping page happens to share a physical page with a zone page containing socket PCBs, the OOB read can access the PCB data.

## Physical OOB Read

`physical_oob_read_mo(memoryObject, offset, size, OOB_OFFSET, readBuffer)`:

1. Sets up: `iov = {pcAddress + 0x3f00, OOB_OFFSET + size}` (scatter-gather into IOSurface mapping)
2. Plants `randomMarker` at `pcAddress + 0x3f00 + OOB_OFFSET` as a canary
3. Race loop:
   - Signals free thread (`raceSync = 1`)
   - `pwritev(readFd, &iov, 1, 0x3f00)` — writes from pcAddress into temp file
   - Free thread does `mach_vm_map(pcAddress, VM_FLAGS_OVERWRITE, targetObject, offset)` — remaps pcAddress to the search mapping's physical page
   - Restores original mapping with `mach_vm_map(pcObject)`
   - If `pwritev` failed (returns -1, mapping changed mid-flight): reads back from the file
   - If read-back differs from `randomMarker` → race won, buffer contains OOB physical data

The `readBuffer` now contains 0xF00 bytes of physical page data starting at offset 0x100. This typically contains 1-4 socket PCBs (each 0x400 bytes).

## PCB Identification

The exploit searches `readBuffer` for the process name ("Runner") using `memmem`. The process name is stored in `inp_last_proc_name` within each `inpcb`. When found:

1. Aligns to 0x400-byte boundary to find the PCB base
2. Validates: `in6p_icmp6filt` and `inp_list.le_prev` must be kernel pointers (top byte 0xFF), `inp_gencnt` must be reasonable (< 0x100000)
3. Matches `inp_gencnt` against the array collected during spray via `proc_info` — this gives the exact index in `socketPorts[]`
4. Rejects if gencnt is the last socket (no adjacent socket for rw pair) or was seen before

## PCB Corruption (The KRW Setup)

### What Gets Corrupted

The exploit overwrites the target PCB's `in6p_icmp6filt` field (offset 0x148):

```
BEFORE:  inpcb.in6p_icmp6filt → data.kalloc.32 allocation (the real filter)
AFTER:   inpcb.in6p_icmp6filt → adjacent_inpcb + 0x148 (the ADJACENT pcb's filter field)
```

This means:
- `setsockopt(corruptedSocket, ICMP6_FILTER, data, 32)` writes 32 bytes to `adjacent_inpcb + 0x148`
- The first 8 bytes of the write become the adjacent socket's `in6p_icmp6filt` pointer
- `getsockopt(adjacentSocket, ICMP6_FILTER, buf, 32)` reads from wherever the adjacent socket's filter now points

### Physical OOB Write

`physical_oob_write_mo(memoryObject, offset, writeSize, OOB_OFFSET, writeBuffer)`:

Same race but in reverse:
1. `pwrite(writeFd, buffer, writeSize, 0x3f00 + OOB_OFFSET)` — writes our data to temp file
2. `preadv(writeFd, &iov, 1, 0x3f00)` — reads from file into pcAddress mapping
3. Free thread remaps pcAddress to target physical page → data lands on the target page

The write is repeated 20 times per call (retry loop), and the corruption loop calls it up to 1000 times until the OOB read-back confirms the corruption took effect.

**Write size**: reduced to `pcbStartOffset + OFFSET_ICMP6FILT + 0x10` (typically 0x158 bytes) to avoid corrupting neighboring PCBs in the same physical page. The full 0xF00-byte write would overwrite 3 adjacent PCBs as collateral damage.

### Socket Pair

After corruption, the exploit identifies the corrupted socket by its `inp_gencnt` (matched against the spray array) and directly opens it and its adjacent socket:

- **controlSocket** = `socketPorts[gencnt_index]` — the socket whose filter was corrupted
- **rwSocket** = `socketPorts[gencnt_index + 1]` — the adjacent socket whose filter field is the corruption target

## KRW Primitive

### Read (`early_kread64(where)`)

1. `set_target_kaddr(aligned_where)`:
   - `setsockopt(controlSocket, ICMP6_FILTER, [aligned_where, 0, 0, 0], 32)`
   - Kernel writes 32 bytes to `controlSocket->in6p_icmp6filt` = `rwSocketPcb + 0x148`
   - First 8 bytes: `aligned_where` → this **sets** `rwSocket->in6p_icmp6filt = aligned_where`
   - Bytes 8-31: zeros → collateral zeroing of `rwSocketPcb + 0x150-0x167`

2. `getsockopt(rwSocket, ICMP6_FILTER, buf, &len)`:
   - Kernel reads from `rwSocket->in6p_icmp6filt` (= `aligned_where`)
   - Returns 32 bytes from `aligned_where`

3. Extract 8 bytes at the correct offset within the 32-byte window

The alignment (`where & ~0x1F`) is needed because `getsockopt` validates that `filter_ptr + sizeof(icmp6_filter)` (= `filter_ptr + 32`) fits within the zone element. Without alignment, reads near the end of small zone elements (e.g., ucred at 144 bytes) would overflow and panic.

### Write (`early_kwrite64(where, what)`)

1. Read 32 bytes from `aligned_where` (same as read path above)
2. Patch 8 bytes at the target offset within the 32-byte buffer
3. `setsockopt(rwSocket, ICMP6_FILTER, patched_buf, 32)`:
   - Kernel writes 32 bytes to `rwSocket->in6p_icmp6filt` (= `aligned_where`)
   - This is a read-modify-write: preserves the other 24 bytes

### Important: 32-byte atomic unit

Both `setsockopt` and `getsockopt` for `ICMP6_FILTER` always operate on exactly `sizeof(struct icmp6_filter)` = 32 bytes. Every KRW operation reads or writes a 32-byte window.

## Socket Leaking

`soclose()` (called when a socket fd is closed or its port is deallocated) unconditionally calls `pru_detach` (= `rip6_detach`) **regardless of `so_usecount`**:

```c
// uipc_socket.c line 1306
if (so->so_pcb != NULL && !(so->so_flags & SOF_PCBCLEARING)) {
    (*so->so_proto->pr_usrreqs->pru_detach)(so);  // ALWAYS called
}
```

`rip6_detach` frees the `icmp6_filter`:
```c
// raw_ip6.c line 1044
if (inp->in6p_icmp6filt) {
    kfree_type(struct icmp6_filter, inp->in6p_icmp6filt);  // zone_require here
}
```

`kfree_type` calls `zone_require` to validate the pointer belongs to the correct kalloc zone. If the filter was corrupted to point elsewhere → **kernel panic**.

### How `so_retaincnt` prevents rip6_detach

`soclose()` (line 1342 of uipc_socket.c) checks `so_retaincnt` BEFORE entering `soclose_locked`:

```c
if (so->so_retaincnt == 0) {
    error = soclose_locked(so);    // calls pru_detach → rip6_detach → kfree_type
} else {
    so->so_usecount--;             // just decrement, skip soclose_locked entirely
}
```

When `so_retaincnt > 0`, `soclose` NEVER calls `soclose_locked`, so `pru_detach` is never reached. The PCB and filter are NOT freed. `so_usecount` alone does NOT prevent `pru_detach` — it's the `so_retaincnt` bump that matters.

### Current approach

The exploit bumps BOTH `so_usecount` AND `so_retaincnt` (via a single 8-byte write to offset 0x254 covering both adjacent 4-byte fields) for controlSocket, rwSocket, and 3 collateral sockets. The `so_retaincnt` bump prevents `soclose_locked` from running, which prevents `rip6_detach` and `kfree_type`. The exploit then calls `sockets_release` to free all 22K socket ports. Sockets with bumped retaincnt survive — `soclose` just decrements usecount and returns.

## The Outstanding Bug

The leak mechanism works correctly for bumped sockets: `so_retaincnt > 0` causes `soclose` to skip `soclose_locked` entirely, so `rip6_detach` and `kfree_type` never run on controlSocket, rwSocket, or the 3 collateral sockets.

However, `sockets_release` frees the ~22K OTHER sockets (those whose only reference was the port, and whose retaincnt was NOT bumped). For these, `soclose → soclose_locked → pru_detach → rip6_detach → kfree_type(in6p_icmp6filt)` runs normally. Their filters should be valid kalloc.32 pointers → `zone_require` should pass.

But the crash shows a socket whose filter points to the `socket` zone — NOT a kalloc zone. This socket was NOT one we corrupted or bumped (different address from controlSocket/rwSocket/collateral). The question is: **how did an unrelated socket's `in6p_icmp6filt` end up pointing to the socket zone?**

Possible causes:
1. **Zone page recycling**: between the OOB read and `sockets_release`, some socket's original `data.kalloc.32` filter allocation is freed (because `rip6_detach` ran on another socket that shared a zone page), and the freed memory is recycled to the socket zone. The remaining socket's filter pointer now dangles into the socket zone. When `sockets_release` frees this socket → `kfree_type` → `zone_require` finds the pointer in the wrong zone
2. **Non-deterministic OOB write**: the 20 retry iterations in `physical_oob_write_mo` each race `preadv` against `mach_vm_map`. Some iterations may land data on unexpected physical pages if the race timing is different, corrupting PCBs we didn't intend to touch
3. **Stale collateral addresses**: the collateral socket addresses read from the OOB buffer may be stale by the time `krw_sockets_leak_forever` runs, causing refcount bumps to write to freed/recycled memory (though this would more likely cause a "modified after free" panic rather than a zone mismatch)

## Kernel Struct Offsets (22B91 / iPhone13,2 / iOS 18.1.1)

### inpcb (1024 bytes, `kalloc.type.1024`)
| Offset | Field | Size |
|--------|-------|------|
| 0x00 | `inpcb_mtx` | 16 |
| 0x10 | `inp_hash` (LIST_ENTRY) | 16 |
| 0x20 | `inp_list.le_next` | 8 |
| 0x28 | `inp_list.le_prev` | 8 |
| 0x30 | `inp_ppcb` | 8 |
| 0x38 | `inp_pcbinfo` | 8 |
| 0x40 | `inp_socket` | 8 |
| 0x78 | `inp_gencnt` | 8 |
| 0x88 | `inp_state` | 4 |
| 0xAD | `inp_vflag` | 1 |
| 0x148 | `inp_depend6.inp6_icmp6filt` | 8 |
| 0x150 | `inp_depend6.inp6_cksum` | 4 |
| 0x154 | `inp_depend6.inp6_hops` | 2 |

### socket (`socket` zone)
| Offset | Field | Size |
|--------|-------|------|
| 0x00 | `so_zone` | 4 |
| 0x04 | `so_type` | 2 |
| 0x06 | `so_protocol` | 2 |
| 0x08 | `so_family` | 4 |
| 0x18 | `so_pcb` | 8 |
| 0x20 | `so_proto` | 8 |
| 0x208 | `so_cred` | 8 |
| 0x248 | `so_flags` | 4 |
| 0x254 | `so_usecount` | 4 |
| 0x258 | `so_retaincnt` | 4 |

### protosw
| Offset | Field | Size |
|--------|-------|------|
| 0x10 | `pr_domain` | 8 |
| 0x28 | `pr_input` | 8 |
| 0x48 | `pr_usrreqs` | 8 |

### ucred (144 bytes, `cred` zone)
| Offset | Field | Size |
|--------|-------|------|
| 0x00 | `cr_rw` | 8 |
| 0x10 | `cr_ref` | 8 |
| 0x18 | `cr_posix.cr_uid` | 4 |
| 0x78 | `cr_label` (PAC-signed) | 8 |

### sandbox extension hash table
| Structure | Offset | Field |
|-----------|--------|-------|
| sandbox | +0x10 | `ext_set` (hash table) |
| ext_set | +0x00 | `ext_table[0]` (read-write bucket) |
| ext_hdr | +0x00 | `next` (linked list) |
| ext_hdr | +0x08 | `ext_lst` (pointer to extension) |
| extension | +0x40 | `data` (path string pointer) |
| extension | +0x48 | `data_length` |

## PAC Handling

iPhone 12 (A14) uses arm64e with PAC. The kernel uses a 40-bit virtual address space (T1SZ=24).

- **Function pointers** (e.g., `pr_input`): PAC-signed. Upper 24 bits are PAC signature. Strip with sign-extension from bit 39.
- **Data pointers** (e.g., `inp_socket`, `so_proto`): NOT PAC-signed on this kernel.
- **`cr_label`**: PAC-signed with `OS_PTRAUTH_SIGNED_PTR_AUTH_NULL`. Must be stripped.

Software `__xpaci`:
```c
if (a & (1ULL << 39))
    return a | 0xFFFFFF0000000000ULL;  // kernel address (bit 39 set)
else
    return a & 0x000000FFFFFFFFFFULL;  // userspace address (bit 39 clear)
```

## TVM Integration

The exploit runs under TVM (Thread-based process Virtualization framework) on iOS. TVM intercepts libc functions at the symbol level. Several TVM bugs were fixed to make the exploit work:

| Function | Issue | Fix |
|----------|-------|-----|
| `pread`/`pwrite`/`preadv`/`pwritev` | Not interposed → called host libc with virtual fds → EBADF | Added FUNCTION_DEFINE with fd translation |
| `fileport_makeport` | Not interposed → passed virtual fd to kernel → failed | Added FUNCTION_DEFINE with CURRENT_RFD |
| `fileport_makefd` | Not interposed → kernel fd not wrapped → stale mapping | Added FUNCTION_DEFINE with task_new_fd |
| `getpid` | Returns TVM virtual PID | Use `syscall(20)` for real PID (syscall not interposed) |

## Sandbox Escape

After KRW is established:

1. Read credential chain: `controlSocketPcb + 0x40` → `socket` → `socket + 0x208` → `ucred` → `ucred + 0x78` → `cr_label` (PAC-strip) → `cr_label + 0x10` → `sandbox`
2. Issue a sandbox extension: `sandbox_extension_issue_file("com.apple.app-sandbox.read-write", tmppath, 0)` + `sandbox_extension_consume(token)` — creates an extension entry in the kernel
3. Walk the extension hash table: `sandbox + 0x10` → `ext_set` → `ext_table[0]` → walk `ext_hdr` chain → `ext_lst` → extension struct
4. Zero `extension + 0x40` (data pointer) and `extension + 0x48` (data length) — makes the extension match any path (wildcard)
5. Verify: `opendir("/var/mobile/Library/AddressBook")` succeeds (was blocked before)

## Build & Run

```sh
# Compile
xcrun --sdk iphoneos cc -arch arm64 -miphoneos-version-min=15.0 -g \
    -Wno-incompatible-pointer-types-discards-qualifiers \
    -Wno-deprecated-declarations \
    -o output/ios-arm64/bin/darksword \
    examples/darksword.c \
    -framework CoreFoundation -framework IOSurface

# Requires TVM app with pread/pwrite/fileport fixes
python3 build.py -a ios-arm64 -t flutter
xcrun devicectl device install app --device pan1c output/ios-arm64/lib/flutter
xcrun devicectl device process launch --terminate-existing --device pan1c com.yuvalino.tvm

# Transfer and run (base64 because raw cat truncates at 48KB in TVM)
base64 -i output/ios-arm64/bin/darksword | \
    sshpass -p alpine ssh -p 2222 mobile@pan1c.local 'base64 -d > darksword && chmod +x darksword'
sshpass -p alpine ssh -p 2222 mobile@pan1c.local './darksword'
```
