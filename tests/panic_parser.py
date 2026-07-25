"""
Parser for iOS kernel panic .ips files.

Extracts panic string, kernel slide, backtrace (unslided), registers,
and panicked task info into a structured dict.
"""

import json
import re


UNSLID_KC_BASE = 0xFFFFFFF007004000


def parse_panic_ips(path):
    """Parse a panic-full-*.ips file. Returns a dict with key fields."""
    with open(path) as f:
        content = f.read()

    # The file is two concatenated JSON objects: header + body.
    # Find the body (starts after the first closing brace of the header line).
    first_newline = content.index("\n")
    header = json.loads(content[:first_newline])
    body = json.loads(content[first_newline + 1:])

    ps = body.get("panicString", "")

    result = {
        "timestamp": header.get("timestamp", ""),
        "os_version": header.get("os_version", ""),
        "incident_id": header.get("incident_id", ""),
    }

    # panic reason (first line of panicString)
    lines = ps.split("\\n") if "\\n" in ps else ps.split("\n")
    result["panic_reason"] = lines[0] if lines else ""

    # kernel slide + base
    kc_slide_m = re.search(r"KernelCache slide:\s+(0x[0-9a-fA-F]+)", ps)
    kc_base_m = re.search(r"KernelCache base:\s+(0x[0-9a-fA-F]+)", ps)
    kt_base_m = re.search(r"Kernel text base:\s+(0x[0-9a-fA-F]+)", ps)

    kc_slide = int(kc_slide_m.group(1), 16) if kc_slide_m else 0
    kc_base = int(kc_base_m.group(1), 16) if kc_base_m else 0
    kt_base = int(kt_base_m.group(1), 16) if kt_base_m else 0

    result["kernelcache_slide"] = kc_slide
    result["kernelcache_base"] = kc_base
    result["kernel_text_base"] = kt_base

    # panicked task
    task_m = re.search(
        r"Panicked task\s+(0x[0-9a-fA-F]+):.*?pid\s+(\d+):\s+(\S+)", ps
    )
    if task_m:
        result["panicked_pid"] = int(task_m.group(2))
        result["panicked_process"] = task_m.group(3)
    else:
        result["panicked_pid"] = None
        result["panicked_process"] = None

    # panicked thread + backtrace
    thread_m = re.search(
        r"Panicked thread:\s+(0x[0-9a-fA-F]+).*?tid:\s+(\d+)", ps
    )
    if thread_m:
        result["panicked_thread"] = thread_m.group(1)
        result["panicked_tid"] = int(thread_m.group(2))
    else:
        result["panicked_thread"] = None
        result["panicked_tid"] = None

    # extract LR backtrace and unslide
    lr_addrs = re.findall(r"lr:\s+(0x[0-9a-fA-F]+)", ps)
    bt_raw = [int(a, 16) for a in lr_addrs]
    bt_unslid = [UNSLID_KC_BASE + (a - kc_base) for a in bt_raw] if kc_base else bt_raw

    result["backtrace_raw"] = bt_raw
    result["backtrace_unslid"] = bt_unslid

    # core registers (from the panicked core)
    result["registers"] = {}
    # TPIDR
    tpidr_m = re.search(
        r"TPIDRx_ELy\s*=\s*\{([^}]+)\}", ps
    )
    if tpidr_m:
        result["registers"]["TPIDRx_ELy"] = tpidr_m.group(1).strip()

    # per-core PC/LR/FP for the panicked core
    panicked_core_m = re.search(r"CORE\s+(\d+)\s+is the one that panicked", ps)
    if panicked_core_m:
        core_id = panicked_core_m.group(1)
        core_regs_m = re.search(
            rf"CORE\s+{core_id}:\s+PC=(0x[0-9a-fA-F]+),\s+LR=(0x[0-9a-fA-F]+),\s+FP=(0x[0-9a-fA-F]+)",
            ps,
        )
        if core_regs_m:
            result["registers"]["PC"] = core_regs_m.group(1)
            result["registers"]["LR"] = core_regs_m.group(2)
            result["registers"]["FP"] = core_regs_m.group(3)
            result["panicked_core"] = int(core_id)

    return result


def format_panic_summary(parsed):
    """Format parsed panic into a human-readable multi-line summary."""
    lines = []
    lines.append(f"PANIC: {parsed['panic_reason']}")
    if parsed.get("panicked_process"):
        lines.append(
            f"Task: pid {parsed['panicked_pid']} ({parsed['panicked_process']})"
        )
    lines.append(f"KernelCache slide: {parsed['kernelcache_slide']:#x}")

    if parsed.get("registers"):
        regs = parsed["registers"]
        if "PC" in regs:
            lines.append(f"Core {parsed.get('panicked_core', '?')}: "
                         f"PC={regs['PC']} LR={regs['LR']} FP={regs['FP']}")

    bt = parsed.get("backtrace_unslid", [])
    if bt:
        lines.append(f"Backtrace (unslid, {len(bt)} frames):")
        for addr in bt[:15]:
            lines.append(f"  {addr:#x}")
        if len(bt) > 15:
            lines.append(f"  ... ({len(bt) - 15} more)")

    return "\n".join(lines)


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <panic-full-*.ips>")
        sys.exit(1)

    parsed = parse_panic_ips(sys.argv[1])
    print(format_panic_summary(parsed))
