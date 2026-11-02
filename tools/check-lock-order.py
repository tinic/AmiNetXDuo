#!/usr/bin/env python3
"""Static lock-order and lock-held-across-blocking-call checker for AmiNetXDuo.

Scans C sources under src/ (or a custom directory for --selftest), builds a
per-function statement/call summary and interprocedural call graph (including
synchronous cross-task job handoffs such as bsd_netstack_run ->
bsd_netstack_boot_main, ami_netstack_rexx_stop -> ami_rx_main, and NetX Duo's
nx_ip_driver_interface_direct_command -> ami_sana2_driver_entry under
nx_ip_protection), and reports:

1. Every A -> B lock acquisition edge with file:line and call chain, plus all
   cycles in the lock-order graph.
2. Every lock held across a blocking call, triaged into:
   - unbounded: reachable from runtime calls with an unbounded wait (e.g.
     OpenDevice on a SANA-II driver under ami_ns_lock, ami_netstack_rexx_stop
     Wait under sb_Lock/ami_ns_lock, nx_ip_protection held across ThreadX baton
     release ami_sana2_block_enter, or tx_thread_sleep called outside the
     ThreadX adoption/baton bracket)
   - bounded: SANA-II bounded I/O deadline or fixed-tick ThreadX wait
   - by_design: first-open startup / last-close teardown under sb_Lock, one-time
     timer.device init/close under ami_timer_lock, or synchronous job Wait in
     bsd_netstack_run / bsd_tcp_handler_start / ami_netstack_rexx_start

Outputs structured key=value lines and exits non-zero when cycles or unbounded
blocking violations are detected (unless --warn-only is passed).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
import re
import sys
import tempfile
from typing import Dict, List, Optional, Sequence, Set, Tuple, Union


C_KEYWORDS = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
    "typeof",
    "__typeof__",
    "__attribute__",
    "__asm__",
    "__asm",
    "asm",
    "case",
    "else",
    "do",
}

EXEC_BLOCKING_CALLS = {
    "Wait",
    "WaitIO",
    "DoIO",
    "OpenDevice",
    "CloseDevice",
    "Delay",
    "EasyRequest",
    "EasyRequestArgs",
    "ami_sana2_open_device_flags",
}

JOB_WAIT_FUNCTIONS = {
    "bsd_netstack_run",
    "bsd_tcp_handler_start",
    "ami_ns_port_start",
}

# ThreadX / NetX Duo calls where a wait timeout argument is passed at the given
# 0-based parameter index.
TX_NX_TIMEOUT_ARG_INDEX: Dict[str, int] = {
    "tx_mutex_get": 1,
    "_tx_mutex_get": 1,
    "tx_semaphore_get": 1,
    "_tx_semaphore_get": 1,
    "tx_event_flags_get": 3,
    "_tx_event_flags_get": 3,
    "tx_queue_receive": 2,
    "_tx_queue_receive": 2,
    "tx_queue_send": 2,
    "_tx_queue_send": 2,
    "tx_byte_allocate": 3,
    "tx_block_allocate": 2,
    "nx_packet_allocate": 3,
    "_nx_packet_allocate": 3,
    "nx_packet_data_append": 4,
    "_nx_packet_data_append": 4,
    "nx_ip_status_check": 2,
    "_nx_ip_status_check": 2,
    "nx_ip_interface_status_check": 3,
    "_nx_ip_interface_status_check": 3,
    "nx_tcp_socket_receive": 2,
    "_nx_tcp_socket_receive": 2,
    "nx_tcp_socket_send": 2,
    "_nx_tcp_socket_send": 2,
    "nx_tcp_client_socket_connect": 3,
    "_nx_tcp_client_socket_connect": 3,
    "nx_tcp_client_socket_bind": 2,
    "_nx_tcp_client_socket_bind": 2,
    "nx_tcp_server_socket_accept": 1,
    "_nx_tcp_server_socket_accept": 1,
    "nx_tcp_socket_disconnect": 1,
    "_nx_tcp_socket_disconnect": 1,
    "nx_udp_socket_receive": 2,
    "_nx_udp_socket_receive": 2,
}

# ThreadX / NetX Duo calls that unconditionally block / sleep.
TX_UNCONDITIONAL_BLOCKING_CALLS = {
    "tx_thread_sleep",
    "_tx_thread_sleep",
    "tx_thread_delete",
    "_tx_thread_delete",
}

# NetX Duo calls that internally acquire ip->nx_ip_protection (TX_WAIT_FOREVER)
# and then invoke the SANA-II link driver entry (ami_sana2_driver_entry).
NX_IP_DRIVER_CALLS_UNDER_IP_MUTEX = {
    "nx_ip_driver_interface_direct_command",
    "_nx_ip_driver_interface_direct_command",
    "nx_ip_driver_direct_command",
    "_nx_ip_driver_direct_command",
    "nx_ip_interface_attach",
    "_nx_ip_interface_attach",
    "nx_ip_interface_detach",
    "_nx_ip_interface_detach",
    "nx_igmp_multicast_interface_join",
    "_nx_igmp_multicast_interface_join",
    "nx_igmp_multicast_interface_leave",
    "_nx_igmp_multicast_interface_leave",
}

# NetX Duo public calls that internally acquire ip->nx_ip_protection with
# TX_WAIT_FOREVER.
NX_IP_MUTEX_INTERNAL_CALL_PREFIXES = (
    "nx_ip_",
    "_nx_ip_",
    "nx_tcp_",
    "_nx_tcp_",
    "nx_udp_",
    "_nx_udp_",
    "nx_arp_",
    "_nx_arp_",
    "nx_igmp_",
    "_nx_igmp_",
    "nx_icmp_",
    "_nx_icmp_",
    "nxd_ipv6_",
    "_nxd_ipv6_",
    "nxd_icmpv6_",
    "_nxd_icmpv6_",
    "nxd_nd_cache_",
    "_nxd_nd_cache_",
    "nxd_udp_",
    "_nxd_udp_",
    "nxd_tcp_",
    "_nxd_tcp_",
)

# Calls matching NX_IP_MUTEX_INTERNAL_CALL_PREFIXES that do NOT take
# nx_ip_protection.
NX_IP_MUTEX_EXEMPT_CALLS = {
    "nx_ip_create",
    "_nx_ip_create",
}


@dataclass(frozen=True)
class Event:
    kind: str
    name: str
    line: int
    detail: str = ""
    conditional: bool = False


@dataclass
class ScopedBlock:
    """A compound statement `{ ... }` inside a function, with early-exit flag."""

    items: List[Union[Event, "ScopedBlock"]] = field(default_factory=list)
    aborts_flow: bool = False


@dataclass
class FunctionSummary:
    name: str
    rel_path: str
    start_line: int
    end_line: int
    root_block: ScopedBlock = field(default_factory=ScopedBlock)


@dataclass(frozen=True)
class HeldLock:
    name: str
    kind: str  # "cpu", "sem", "claim", "slot", "baton", "tx_mutex", "join_wait"
    site: str  # func@file:line


@dataclass(frozen=True)
class LockEdge:
    from_lock: str
    to_lock: str
    file_line: str
    func: str
    chain: str


@dataclass(frozen=True)
class BlockingViolation:
    category: str  # "exec_block", "tx_wait", "job_wait", "baton_release_with_mutex", "unadopted_tx_call"
    triage: str  # "unbounded", "bounded", "by_design"
    held_lock: str
    held_kind: str
    acquired_at: str
    blocking_call: str
    file_line: str
    func: str
    chain: str


def strip_comments_strings_and_directives(source: str) -> str:
    """Replace comments, string/char literals, and #preprocessor lines with spaces, keeping newlines."""
    out: List[str] = []
    i = 0
    n = len(source)
    at_line_start = True
    while i < n:
        ch = source[i]
        nxt = source[i + 1] if i + 1 < n else ""
        if at_line_start and ch == "#":
            while i < n:
                if source[i] == "\\" and i + 1 < n and source[i + 1] == "\n":
                    out.append(" \n")
                    i += 2
                    continue
                if source[i] == "\n":
                    out.append("\n")
                    i += 1
                    break
                out.append(" ")
                i += 1
            at_line_start = True
            continue
        if ch == "/" and nxt == "/":
            out.append("  ")
            i += 2
            while i < n and source[i] != "\n":
                out.append(" ")
                i += 1
        elif ch == "/" and nxt == "*":
            out.append("  ")
            i += 2
            while i < n:
                if source[i] == "*" and (i + 1 < n and source[i + 1] == "/"):
                    out.append("  ")
                    i += 2
                    break
                out.append("\n" if source[i] == "\n" else " ")
                i += 1
        elif ch in ('"', "'"):
            quote = ch
            out.append(" ")
            i += 1
            while i < n:
                c = source[i]
                if c == "\\" and i + 1 < n:
                    out.append("\n" if source[i + 1] == "\n" else "  ")
                    i += 2
                    continue
                if c == quote:
                    out.append(" ")
                    i += 1
                    break
                out.append("\n" if c == "\n" else " ")
                i += 1
            at_line_start = False
        else:
            out.append(ch)
            if ch == "\n":
                at_line_start = True
            elif not ch.isspace():
                at_line_start = False
            i += 1
    return "".join(out)


def build_line_offsets(text: str) -> List[int]:
    offsets = [0]
    for idx, ch in enumerate(text):
        if ch == "\n":
            offsets.append(idx + 1)
    return offsets


def offset_to_line(offsets: Sequence[int], pos: int) -> int:
    lo = 0
    hi = len(offsets) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if offsets[mid] <= pos:
            lo = mid + 1
        else:
            hi = mid - 1
    return hi + 1


def find_matching_delim(text: str, open_pos: int, open_ch: str, close_ch: str) -> int:
    depth = 0
    for i in range(open_pos, len(text)):
        c = text[i]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
    return -1


def find_matching_delim_backward(
    text: str, close_pos: int, open_ch: str, close_ch: str
) -> int:
    depth = 0
    for i in range(close_pos, -1, -1):
        c = text[i]
        if c == close_ch:
            depth += 1
        elif c == open_ch:
            depth -= 1
            if depth == 0:
                return i
    return -1


def split_top_level_args(arg_str: str) -> List[str]:
    args: List[str] = []
    cur: List[str] = []
    depth = 0
    for ch in arg_str:
        if ch in "([{":
            depth += 1
            cur.append(ch)
        elif ch in ")]}":
            depth = max(0, depth - 1)
            cur.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        args.append(tail)
    return args


def normalize_semaphore_name(expr: str) -> str:
    clean = re.sub(r"\s+", "", expr)
    while clean.startswith("(") and clean.endswith(")"):
        inner = clean[1:-1]
        if find_matching_delim(clean, 0, "(", ")") == len(clean) - 1:
            clean = inner
        else:
            break
    clean = re.sub(r"^\([A-Za-z0-9_*]+\)", "", clean)
    clean = clean.lstrip("&")
    if "sb_Lock" in clean:
        return "sb_Lock"
    if "bsd_job_lock" in clean:
        return "bsd_job_lock"
    if "ami_ns_lock" in clean:
        return "ami_ns_lock"
    if "ami_bpf_sem" in clean:
        return "ami_bpf_sem"
    if "ami_timer_lock" in clean:
        return "ami_timer_lock"
    if "pool_sem" in clean:
        return "pool_sem"
    if "tb_Lock" in clean:
        return "tb_Lock"
    if "tm_Semaphore" in clean:
        return "tm_Semaphore"
    if "nd_LifecycleLock" in clean:
        return "nd_LifecycleLock"
    if "nd_PcmciaLock" in clean:
        return "nd_PcmciaLock"
    if "LayerInfo.Lock" in clean:
        return "LayerInfo.Lock"
    if clean.endswith("->lock") or clean == "g->lock":
        return "ug_lock"
    if "->" in clean:
        return clean.split("->")[-1]
    return clean or "unknown_sem"


def normalize_tx_mutex_name(expr: str) -> str:
    clean = re.sub(r"\s+", "", expr)
    clean = clean.lstrip("&")
    if "nx_ip_protection" in clean:
        return "nx_ip_protection"
    if "nx_dns_mutex" in clean:
        return "nx_dns_mutex"
    if "nx_mdns_mutex" in clean:
        return "nx_mdns_mutex"
    if "nx_dhcpv6_client_mutex" in clean:
        return "nx_dhcpv6_client_mutex"
    if "_nx_secure_tls_protection" in clean:
        return "_nx_secure_tls_protection"
    if "->" in clean:
        return clean.split("->")[-1]
    return clean or "tx_mutex"


def is_nonzero_timeout(expr: str) -> bool:
    clean = re.sub(r"\s+", "", expr)
    if not clean:
        return False
    if clean in ("0", "0U", "0UL", "0L", "TX_NO_WAIT", "NX_NO_WAIT"):
        return False
    return True


def extract_functions(clean_text: str, rel_path: str) -> List[FunctionSummary]:
    """Walk top-level `{ ... }` blocks to find every C function definition."""
    offsets = build_line_offsets(clean_text)
    funcs: List[FunctionSummary] = []
    i = 0
    n = len(clean_text)
    while i < n:
        brace_open = clean_text.find("{", i)
        if brace_open < 0:
            break
        brace_close = find_matching_delim(clean_text, brace_open, "{", "}")
        if brace_close < 0:
            break

        j = brace_open - 1
        while j >= 0 and clean_text[j].isspace():
            j -= 1

        while j >= 0 and clean_text[j] == ")":
            open_p = find_matching_delim_backward(clean_text, j, "(", ")")
            if open_p < 0:
                break
            k = open_p - 1
            while k >= 0 and clean_text[k].isspace():
                k -= 1
            ident_end = k + 1
            while k >= 0 and (clean_text[k].isalnum() or clean_text[k] == "_"):
                k -= 1
            ident = clean_text[k + 1 : ident_end]
            if ident in ("__attribute__", "__attribute", "__asm__", "__asm", "asm"):
                j = k
                while j >= 0 and clean_text[j].isspace():
                    j -= 1
            else:
                break

        if j >= 0 and clean_text[j] == ")":
            param_open = find_matching_delim_backward(clean_text, j, "(", ")")
            if param_open > 0:
                k = param_open - 1
                while k >= 0 and clean_text[k].isspace():
                    k -= 1
                ident_end = k + 1
                while k >= 0 and (clean_text[k].isalnum() or clean_text[k] == "_"):
                    k -= 1
                fn_name = clean_text[k + 1 : ident_end]
                prev_char_idx = k
                while prev_char_idx >= 0 and clean_text[prev_char_idx].isspace():
                    prev_char_idx -= 1
                prev_char = clean_text[prev_char_idx] if prev_char_idx >= 0 else ""
                if (
                    fn_name
                    and not fn_name[0].isdigit()
                    and fn_name not in C_KEYWORDS
                    and prev_char not in ("=", ",", "(", ";")
                ):
                    start_line = offset_to_line(offsets, k + 1)
                    end_line = offset_to_line(offsets, brace_close)
                    root_block = parse_scoped_block(
                        fn_name=fn_name,
                        text=clean_text,
                        start_offset=brace_open + 1,
                        end_offset=brace_close,
                        offsets=offsets,
                    )
                    funcs.append(
                        FunctionSummary(
                            name=fn_name,
                            rel_path=rel_path,
                            start_line=start_line,
                            end_line=end_line,
                            root_block=root_block,
                        )
                    )

        i = brace_close + 1
    return funcs


def block_ends_with_abort(block_text: str) -> bool:
    """Return True if the final statement at top-level of `block_text` is return/goto/break/continue."""
    stripped = block_text.rstrip()
    if not stripped:
        return False
    m = re.search(
        r"(?:^|[;\}])\s*(return|goto|break|continue)\b[^;{}]*;\s*$",
        stripped,
    )
    return bool(m)


def parse_scoped_block(
    fn_name: str,
    text: str,
    start_offset: int,
    end_offset: int,
    offsets: Sequence[int],
) -> ScopedBlock:
    block = ScopedBlock(
        aborts_flow=block_ends_with_abort(text[start_offset:end_offset])
    )
    token_re = re.compile(r"\{|\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    i = start_offset
    while i < end_offset:
        m = token_re.search(text, i, end_offset)
        if not m:
            break
        if m.group(0) == "{":
            sub_open = m.start()
            sub_close = find_matching_delim(text, sub_open, "{", "}")
            if sub_close < 0 or sub_close > end_offset:
                i = sub_open + 1
                continue
            sub_block = parse_scoped_block(
                fn_name=fn_name,
                text=text,
                start_offset=sub_open + 1,
                end_offset=sub_close,
                offsets=offsets,
            )
            block.items.append(sub_block)
            i = sub_close + 1
            continue

        callee = m.group(1)
        open_paren = m.end() - 1
        close_paren = find_matching_delim(text, open_paren, "(", ")")
        if close_paren < 0 or close_paren > end_offset:
            i = open_paren + 1
            continue
        if callee in C_KEYWORDS:
            i = open_paren + 1
            continue

        arg_str = text[open_paren + 1 : close_paren]
        args = split_top_level_args(arg_str)
        abs_pos = m.start(1)
        line = offset_to_line(offsets, abs_pos)

        prefix = text[max(start_offset, m.start(1) - 40) : m.start(1)]
        in_if_cond = bool(re.search(r"\bif\s*\([^)]*$", prefix))

        events: List[Event] = []
        classify_call_event(
            fn_name=fn_name,
            callee=callee,
            args=args,
            line=line,
            in_if_cond=in_if_cond,
            events=events,
        )
        block.items.extend(events)
        i = open_paren + 1

    return block


def classify_call_event(
    fn_name: str,
    callee: str,
    args: Sequence[str],
    line: int,
    in_if_cond: bool,
    events: List[Event],
) -> None:
    # 1. Exec CPU locks: Forbid/Permit, Disable/Enable
    if callee == "Forbid":
        events.append(Event("acquire", "Forbid", line, "cpu"))
        return
    if callee == "Permit":
        events.append(Event("release", "Forbid", line, "cpu"))
        return
    if callee == "Disable":
        events.append(Event("acquire", "Disable", line, "cpu"))
        return
    if callee == "Enable":
        events.append(Event("release", "Disable", line, "cpu"))
        return

    # 2. Exec SignalSemaphores
    if callee in ("ObtainSemaphore", "ObtainSemaphoreShared") and args:
        sem_name = normalize_semaphore_name(args[0])
        events.append(Event("acquire", sem_name, line, "sem"))
        return
    if callee in ("AttemptSemaphore", "AttemptSemaphoreShared") and args:
        sem_name = normalize_semaphore_name(args[0])
        events.append(
            Event("try_acquire", sem_name, line, "sem", conditional=True)
        )
        return
    if callee == "ReleaseSemaphore" and args:
        sem_name = normalize_semaphore_name(args[0])
        events.append(Event("release", sem_name, line, "sem"))
        return

    # Known semaphore / lock wrappers so callers get exact lock state transitions
    if callee == "ami_ns_lock_obtain" and fn_name != "ami_ns_lock_obtain":
        events.append(Event("acquire", "ami_ns_lock", line, "sem"))
        return
    if callee == "ami_ns_lock_attempt" and fn_name != "ami_ns_lock_attempt":
        events.append(
            Event("try_acquire", "ami_ns_lock", line, "sem", conditional=True)
        )
        return
    if callee == "ami_ns_lock_release" and fn_name != "ami_ns_lock_release":
        events.append(Event("release", "ami_ns_lock", line, "sem"))
        return
    if callee == "ami_bpf_lock" and fn_name != "ami_bpf_lock":
        events.append(Event("acquire", "ami_bpf_sem", line, "sem"))
        return
    if callee == "ami_bpf_try_lock" and fn_name != "ami_bpf_try_lock":
        events.append(
            Event("try_acquire", "ami_bpf_sem", line, "sem", conditional=True)
        )
        return
    if callee == "ami_bpf_unlock" and fn_name != "ami_bpf_unlock":
        events.append(Event("release", "ami_bpf_sem", line, "sem"))
        return
    if callee == "pool_lock" and fn_name != "pool_lock":
        events.append(Event("acquire", "pool_sem", line, "sem"))
        return
    if callee == "ami_mbuf_lock" and fn_name != "ami_mbuf_lock":
        events.append(Event("acquire", "Forbid", line, "cpu"))
        return
    if callee == "ami_mbuf_unlock" and fn_name != "ami_mbuf_unlock":
        events.append(Event("release", "Forbid", line, "cpu"))
        return
    if callee == "ami_ns_resolver_forbid" and fn_name != "ami_ns_resolver_forbid":
        events.append(Event("acquire", "Forbid", line, "cpu"))
        return
    if callee == "ami_ns_resolver_permit" and fn_name != "ami_ns_resolver_permit":
        events.append(Event("release", "Forbid", line, "cpu"))
        return

    # 3. Interface slot claim (netstack_interface_claim / release)
    # Note: netstack_interface_claim transiently takes and releases ami_ns_lock
    # to bump the ns_IfaceClaims refcount, and netstack_interface_remove_begin
    # checks ns_IfaceClaims != 0 non-blockingly (returns AMI_NET_ERR_BUSY), so
    # ami_ns_lock never blocks waiting for ns_IfaceClaims.
    if callee in (
        "netstack_interface_claim",
        "ami_netstack_interface_claim_cookie",
    ) and fn_name not in (
        "netstack_interface_claim",
        "ami_netstack_interface_claim_cookie",
    ):
        events.append(Event("acquire", "ami_ns_lock", line, "sem"))
        events.append(Event("release", "ami_ns_lock", line, "sem"))
        events.append(
            Event("acquire", "ns_IfaceClaims", line, "claim", conditional=in_if_cond)
        )
        return
    if callee == "netstack_interface_release" and fn_name != "netstack_interface_release":
        events.append(Event("acquire", "ami_ns_lock", line, "sem"))
        events.append(Event("release", "ami_ns_lock", line, "sem"))
        events.append(Event("release", "ns_IfaceClaims", line, "claim"))
        return

    # 4. Adoption slot + ThreadX baton brackets
    if callee in (
        "bsd_nx_enter",
        "bsd_nx_need",
        "ami_netstack_enter",
        "ami_netstack_enter_alloc",
        "ami_netstack_enter_cached",
    ) and fn_name != "bsd_nx_need":
        events.append(
            Event("reentrant_nx_enter", callee, line, "baton", conditional=in_if_cond)
        )
        return
    if callee in (
        "bsd_nx_leave",
        "ami_netstack_leave",
        "ami_netstack_leave_free",
        "ami_netstack_leave_cached",
    ):
        events.append(Event("reentrant_nx_leave", callee, line, "baton"))
        return
    if callee in (
        "tx_amiga_adopt_thread",
        "tx_amiga_adopt_thread_break",
        "tx_amiga_adopt_resume",
    ):
        events.append(
            Event("acquire", "adopt_slot", line, "slot", conditional=in_if_cond)
        )
        return
    if callee in ("tx_amiga_orphan_thread", "tx_amiga_adopt_suspend"):
        events.append(Event("release", "adopt_slot", line, "slot"))
        return
    if callee in ("ami_sana2_block_enter", "ami_netstack_baton_release"):
        events.append(Event("baton_release", callee, line, "baton"))
        return
    if callee in ("ami_sana2_block_leave", "ami_netstack_baton_acquire"):
        events.append(Event("baton_acquire", callee, line, "baton"))
        return
    if callee == "bsd_nx_park":
        events.append(Event("park", callee, line, "baton"))
        return

    # 5. ThreadX mutexes
    if callee in ("tx_mutex_get", "_tx_mutex_get") and args:
        mtx = normalize_tx_mutex_name(args[0])
        timeout_arg = args[1] if len(args) > 1 else "TX_WAIT_FOREVER"
        events.append(
            Event(
                "tx_mutex_acquire",
                mtx,
                line,
                timeout_arg if is_nonzero_timeout(timeout_arg) else "",
                conditional=in_if_cond,
            )
        )
        return
    if callee in ("tx_mutex_put", "_tx_mutex_put") and args:
        mtx = normalize_tx_mutex_name(args[0])
        events.append(Event("release", mtx, line, "tx_mutex"))
        return

    # 6. Synchronous job Wait functions (bsd_netstack_run, etc.)
    if callee in JOB_WAIT_FUNCTIONS:
        events.append(
            Event("job_wait_call", callee, line, args[1] if len(args) > 1 else "")
        )
        events.append(Event("call", callee, line, args[1] if len(args) > 1 else ""))
        return

    # Callback pointer ami_shutdown_notify() -> bsd_shutdown_requested()
    if callee == "ami_shutdown_notify" and fn_name != "ami_shutdown_notify":
        events.append(Event("call", "bsd_shutdown_requested", line))
        return

    # 7. NetX Duo calls that take nx_ip_protection and call ami_sana2_driver_entry
    if callee in NX_IP_DRIVER_CALLS_UNDER_IP_MUTEX:
        cmd_arg = args[1] if len(args) > 1 else ""
        events.append(Event("nx_driver_call", callee, line, cmd_arg.strip()))
        return

    # 8. Other NetX Duo calls that internally acquire nx_ip_protection
    if (
        callee.startswith(NX_IP_MUTEX_INTERNAL_CALL_PREFIXES)
        and callee not in NX_IP_MUTEX_EXEMPT_CALLS
    ):
        if callee in TX_NX_TIMEOUT_ARG_INDEX:
            idx = TX_NX_TIMEOUT_ARG_INDEX[callee]
            if len(args) > idx and is_nonzero_timeout(args[idx]):
                events.append(
                    Event("block_tx", f"{callee}(timeout={args[idx]})", line, "tx_wait")
                )
        events.append(Event("nx_ip_call", callee, line))
        return

    # 9. ThreadX / NetX Duo blocking calls with timeout != 0 or unconditional sleep
    if callee in TX_UNCONDITIONAL_BLOCKING_CALLS:
        arg0 = args[0] if args else ""
        if callee in ("tx_thread_sleep", "_tx_thread_sleep") and not is_nonzero_timeout(
            arg0
        ):
            return
        events.append(Event("block_tx", f"{callee}({arg0})", line, "tx_wait"))
        return

    if callee in TX_NX_TIMEOUT_ARG_INDEX:
        idx = TX_NX_TIMEOUT_ARG_INDEX[callee]
        if len(args) > idx and is_nonzero_timeout(args[idx]):
            events.append(
                Event("block_tx", f"{callee}(timeout={args[idx]})", line, "tx_wait")
            )
        return

    # 10. Exec blocking calls
    if callee in EXEC_BLOCKING_CALLS:
        events.append(Event("block_exec", callee, line, "exec_block"))
        if callee == "ami_sana2_open_device_flags":
            events.append(Event("call", callee, line))
        return

    # 11. General function call within src/
    events.append(Event("call", callee, line, args[1] if len(args) > 1 else ""))


NETSTACK_OP_TARGETS: Dict[str, str] = {
    "BSD_NETSTACK_OP_STARTUP": "netstack_startup_loopback",
    "BSD_NETSTACK_OP_IFACE_START": "netstack_interface_start",
    "BSD_NETSTACK_OP_IFACE_UP": "netstack_interface_up",
    "BSD_NETSTACK_OP_IFACE_DOWN": "netstack_interface_down",
    "BSD_NETSTACK_OP_IFACE_STACK_DOWN": "netstack_interface_stack_down",
    "BSD_NETSTACK_OP_IFACE_REMOVE": "netstack_interface_remove",
    "BSD_NETSTACK_OP_IFACE_REMOVE_NAMED": "netstack_interface_remove_named",
}


def classify_triage(
    category: str,
    held: HeldLock,
    blocking_call: str,
    file_line: str,
    chain: str,
) -> str:
    """Classify a lock-held-across-blocking-call hit as unbounded, bounded, or by_design."""
    if category in ("baton_release_with_mutex", "unadopted_tx_call"):
        return "unbounded"

    # Synchronous job Wait in bsd_netstack_run, bsd_tcp_handler_start, or
    # ami_netstack_rexx_start is by design.
    if category == "job_wait" or file_line.endswith("netstack_rexx.c:482") or file_line.endswith("library.c:869"):
        return "by_design"

    # ARexx stop Wait(mask) under ami_ns_lock / sb_Lock can deadlock against
    # ami_rx_main draining KILL/mDNS commands before checking SIGBREAKF_CTRL_C.
    if "netstack_rexx.c:517" in file_line or "netstack_rexx.c:519" in file_line:
        return "unbounded"

    # One-time timer.device open/close under ami_timer_lock / ami_tls_timer_lock / select
    if (
        held.name in ("ami_timer_lock", "ami_tls_timer_lock")
        or "compat.c:218" in file_line
        or "compat.c:267" in file_line
        or "tls_amiga.c:93" in file_line
        or "tls_amiga.c:141" in file_line
        or "select.c:811" in file_line
    ):
        return "by_design"

    # First-open stack bring-up or last-close stack shutdown under sb_Lock /
    # ami_ns_startup / netstack_shutdown (excluding rexx_stop handled above)
    if (
        "bsd_lib_open@" in chain
        or "ami_ns_startup@" in chain
        or "netstack_shutdown@" in chain
        or "bsd_netstack_shutdown_owned@" in chain
    ):
        return "by_design"

    # nx_ip_protection held across Exec Wait/WaitIO/OpenDevice/CloseDevice or
    # multi-second ThreadX semaphore/sleep in SANA-II driver commands is
    # reachable from runtime link enable/disable/multicast and starves the
    # adoption pool (#383).
    if held.name == "nx_ip_protection":
        return "unbounded"

    # Opening an arbitrary SANA-II device (compat.c:476 / sana2_device.c:1461)
    # under ami_ns_lock / bsd_job_lock in runtime interface add/start is
    # unbounded (can pop an Intuition EasyRequest on missing hardware).
    if "compat.c:476" in file_line or "sana2_device.c:1461" in file_line:
        return "unbounded"

    # SANA-II bounded command helper (sana2_device.c:641,646,701,716,734,340,1746)
    # or fixed-tick ThreadX sleep/semaphore wait when nx_ip_protection is NOT held
    if (
        "sana2_device.c:" in file_line
        or "sana2_rx.c:" in file_line
        or "sana2_tx.c:" in file_line
        or blocking_call.startswith("tx_thread_sleep(")
        or blocking_call.startswith("tx_semaphore_get(")
        or blocking_call.startswith("tx_thread_delete(")
    ):
        return "bounded"

    return "unbounded"


class LockOrderAnalyzer:
    def __init__(self, funcs_by_name: Dict[str, List[FunctionSummary]]) -> None:
        self.funcs_by_name = funcs_by_name
        self.edges: Dict[Tuple[str, str, str, str], LockEdge] = {}
        self.violations: Dict[Tuple[str, str, str, str], BlockingViolation] = {}
        self.locks_discovered: Set[str] = set()

    def record_edge(
        self,
        held: HeldLock,
        new_lock: str,
        new_kind: str,
        fn: FunctionSummary,
        line: int,
        call_stack: Sequence[str],
    ) -> None:
        self.locks_discovered.add(held.name)
        self.locks_discovered.add(new_lock)
        if held.name == new_lock:
            return
        file_line = f"{fn.rel_path}:{line}"
        chain = " -> ".join([held.site, *call_stack, f"{fn.name}@{file_line}"])
        key = (held.name, new_lock, file_line, fn.name)
        if key not in self.edges:
            self.edges[key] = LockEdge(
                from_lock=held.name,
                to_lock=new_lock,
                file_line=file_line,
                func=fn.name,
                chain=chain,
            )

    def record_violation(
        self,
        category: str,
        held: HeldLock,
        blocking_call: str,
        fn: FunctionSummary,
        line: int,
        call_stack: Sequence[str],
    ) -> None:
        file_line = f"{fn.rel_path}:{line}"
        chain = " -> ".join([held.site, *call_stack, f"{fn.name}@{file_line}"])
        triage = classify_triage(category, held, blocking_call, file_line, chain)
        key = (category, held.name, blocking_call, file_line)
        if key not in self.violations or (
            self.violations[key].triage != "unbounded" and triage == "unbounded"
        ):
            self.violations[key] = BlockingViolation(
                category=category,
                triage=triage,
                held_lock=held.name,
                held_kind=held.kind,
                acquired_at=held.site,
                blocking_call=blocking_call,
                file_line=file_line,
                func=fn.name,
                chain=chain,
            )

    def analyze_all(self) -> None:
        for fn_list in self.funcs_by_name.values():
            for fn in fn_list:
                self.analyze_function(
                    fn=fn,
                    initial_locks=[],
                    call_stack=[],
                    visited_funcs={fn.name},
                    depth=0,
                )

    def resolve_callee(
        self, caller: FunctionSummary, callee_name: str
    ) -> Optional[FunctionSummary]:
        candidates = self.funcs_by_name.get(callee_name)
        if not candidates:
            return None
        for cand in candidates:
            if cand.rel_path == caller.rel_path:
                return cand
        return candidates[0]

    def analyze_function(
        self,
        fn: FunctionSummary,
        initial_locks: Sequence[HeldLock],
        call_stack: Sequence[str],
        visited_funcs: Set[str],
        depth: int,
    ) -> List[HeldLock]:
        if depth > 14:
            return list(initial_locks)
        return self.analyze_block(
            fn=fn,
            block=fn.root_block,
            initial_locks=initial_locks,
            call_stack=call_stack,
            visited_funcs=visited_funcs,
            depth=depth,
            reentrant_nx_depth=[0],
        )

    def analyze_block(
        self,
        fn: FunctionSummary,
        block: ScopedBlock,
        initial_locks: Sequence[HeldLock],
        call_stack: Sequence[str],
        visited_funcs: Set[str],
        depth: int,
        reentrant_nx_depth: List[int],
    ) -> List[HeldLock]:
        held: List[HeldLock] = list(initial_locks)

        for item in block.items:
            if isinstance(item, ScopedBlock):
                saved_held = list(held)
                saved_nx_depth = reentrant_nx_depth[0]
                after_sub = self.analyze_block(
                    fn=fn,
                    block=item,
                    initial_locks=held,
                    call_stack=call_stack,
                    visited_funcs=visited_funcs,
                    depth=depth,
                    reentrant_nx_depth=reentrant_nx_depth,
                )
                if item.aborts_flow:
                    held = saved_held
                    reentrant_nx_depth[0] = saved_nx_depth
                else:
                    held = after_sub
                continue

            ev = item
            file_line = f"{fn.rel_path}:{ev.line}"
            site = f"{fn.name}@{file_line}"

            if ev.kind in ("acquire", "try_acquire"):
                self.locks_discovered.add(ev.name)
                for h in held:
                    self.record_edge(h, ev.name, ev.detail, fn, ev.line, call_stack)
                    if (
                        ev.kind == "acquire"
                        and ev.detail == "sem"
                        and h.kind == "cpu"
                    ):
                        self.record_violation(
                            "exec_block",
                            h,
                            f"ObtainSemaphore({ev.name})",
                            fn,
                            ev.line,
                            call_stack,
                        )
                if not any(h.name == ev.name for h in held):
                    held.append(HeldLock(name=ev.name, kind=ev.detail, site=site))

            elif ev.kind == "tx_mutex_acquire":
                mtx = ev.name
                self.locks_discovered.add(mtx)
                already_held = any(h.name == mtx for h in held)
                if not already_held:
                    if ev.detail:
                        for h in held:
                            if h.kind in ("cpu", "sem", "tx_mutex", "claim"):
                                self.record_violation(
                                    "tx_wait",
                                    h,
                                    f"tx_mutex_get({mtx}, {ev.detail})",
                                    fn,
                                    ev.line,
                                    call_stack,
                                )
                    for h in held:
                        self.record_edge(h, mtx, "tx_mutex", fn, ev.line, call_stack)
                    held.append(HeldLock(name=mtx, kind="tx_mutex", site=site))

            elif ev.kind == "reentrant_nx_enter":
                if any(h.name == "adopt_slot" for h in held):
                    reentrant_nx_depth[0] += 1
                else:
                    self.locks_discovered.add("adopt_slot")
                    self.locks_discovered.add("netx_baton")
                    for h in held:
                        self.record_edge(
                            h, "adopt_slot", "slot", fn, ev.line, call_stack
                        )
                    held.append(
                        HeldLock(name="adopt_slot", kind="slot", site=site)
                    )
                    for h in held:
                        self.record_edge(
                            h, "netx_baton", "baton", fn, ev.line, call_stack
                        )
                    held.append(
                        HeldLock(name="netx_baton", kind="baton", site=site)
                    )

            elif ev.kind == "reentrant_nx_leave":
                if reentrant_nx_depth[0] > 0:
                    reentrant_nx_depth[0] -= 1
                else:
                    held = [
                        h
                        for h in held
                        if h.name not in ("netx_baton", "adopt_slot")
                    ]

            elif ev.kind == "release":
                held = [h for h in held if h.name != ev.name]

            elif ev.kind == "baton_release":
                for h in held:
                    if h.kind == "tx_mutex":
                        self.record_violation(
                            "baton_release_with_mutex",
                            h,
                            ev.name,
                            fn,
                            ev.line,
                            call_stack,
                        )
                held = [h for h in held if h.name != "netx_baton"]

            elif ev.kind == "baton_acquire":
                if not any(h.name == "netx_baton" for h in held):
                    held.append(HeldLock(name="netx_baton", kind="baton", site=site))

            elif ev.kind == "park":
                for h in held:
                    if h.name not in ("netx_baton", "adopt_slot"):
                        self.record_violation(
                            "exec_block",
                            h,
                            "bsd_nx_park(Wait)",
                            fn,
                            ev.line,
                            call_stack,
                        )

            elif ev.kind == "block_exec":
                for h in held:
                    if h.kind in ("cpu", "sem", "tx_mutex", "baton", "claim"):
                        self.record_violation(
                            "exec_block",
                            h,
                            ev.name,
                            fn,
                            ev.line,
                            call_stack,
                        )

            elif ev.kind == "block_tx":
                # Check if a ThreadX sleep/wait is invoked under a semaphore
                # while NOT adopted into the ThreadX baton bracket!
                is_adopted = any(h.name in ("adopt_slot", "netx_baton", "nx_ip_protection") for h in held)
                for h in held:
                    if h.kind in ("cpu", "sem", "tx_mutex", "claim"):
                        cat = (
                            "unadopted_tx_call"
                            if (not is_adopted and ev.name.startswith("tx_thread_sleep("))
                            else "tx_wait"
                        )
                        self.record_violation(
                            cat,
                            h,
                            ev.name,
                            fn,
                            ev.line,
                            call_stack,
                        )

            elif ev.kind == "job_wait_call":
                for h in held:
                    if h.kind in ("cpu", "sem", "tx_mutex", "baton", "claim"):
                        self.record_violation(
                            "job_wait",
                            h,
                            ev.name,
                            fn,
                            ev.line,
                            call_stack,
                        )

            elif ev.kind == "nx_ip_call":
                self.locks_discovered.add("nx_ip_protection")
                for h in held:
                    self.record_edge(
                        h, "nx_ip_protection", "tx_mutex", fn, ev.line, call_stack
                    )

            elif ev.kind == "nx_driver_call":
                self.locks_discovered.add("nx_ip_protection")
                for h in held:
                    self.record_edge(
                        h, "nx_ip_protection", "tx_mutex", fn, ev.line, call_stack
                    )
                ip_lock = HeldLock(
                    name="nx_ip_protection",
                    kind="tx_mutex",
                    site=f"{ev.name}@{file_line}",
                )
                driver_fn = self.resolve_callee(fn, "ami_sana2_driver_command")
                if driver_fn and driver_fn.name not in visited_funcs:
                    self.analyze_function(
                        fn=driver_fn,
                        initial_locks=[*held, ip_lock],
                        call_stack=[*call_stack, f"{fn.name}:{ev.name}@{file_line}"],
                        visited_funcs=visited_funcs | {driver_fn.name},
                        depth=depth + 1,
                    )

            elif ev.kind == "call":
                if ev.name == "bsd_netstack_run":
                    job_lock = HeldLock(
                        name="bsd_job_lock",
                        kind="sem",
                        site=f"bsd_netstack_run@{file_line}",
                    )
                    for h in held:
                        self.record_edge(
                            h, "bsd_job_lock", "sem", fn, ev.line, call_stack
                        )
                    op_expr = ev.detail.strip()
                    matched_ops = re.findall(r"BSD_NETSTACK_OP_[A-Z_]+", op_expr)
                    targets = (
                        [
                            NETSTACK_OP_TARGETS[op]
                            for op in matched_ops
                            if op in NETSTACK_OP_TARGETS
                        ]
                        if matched_ops
                        else list(NETSTACK_OP_TARGETS.values())
                    )
                    for tgt in targets:
                        tgt_fn = self.resolve_callee(fn, tgt)
                        if tgt_fn and tgt_fn.name not in visited_funcs:
                            self.analyze_function(
                                fn=tgt_fn,
                                initial_locks=[*held, job_lock],
                                call_stack=[
                                    *call_stack,
                                    f"{fn.name}:bsd_netstack_run({','.join(matched_ops) or '*'})@{file_line}",
                                ],
                                visited_funcs=visited_funcs | {tgt_fn.name},
                                depth=depth + 1,
                            )
                    continue

                if ev.name == "ami_netstack_rexx_stop":
                    # ami_netstack_rexx_stop signals SIGBREAKF_CTRL_C and blocks
                    # synchronously in Wait(mask) until ami_rx_main completes
                    # ami_rx_drain(port, FALSE) -> ami_rx_service -> ami_rx_execute.
                    rexx_wait_lock = HeldLock(
                        name="rexx_stop_wait",
                        kind="join_wait",
                        site=f"ami_netstack_rexx_stop@{file_line}",
                    )
                    for h in held:
                        self.record_edge(
                            h, "rexx_stop_wait", "join_wait", fn, ev.line, call_stack
                        )
                    rexx_fn = self.resolve_callee(fn, "ami_netstack_rexx_stop")
                    if rexx_fn and rexx_fn.name not in visited_funcs and held:
                        self.analyze_function(
                            fn=rexx_fn,
                            initial_locks=held,
                            call_stack=[*call_stack, site],
                            visited_funcs=visited_funcs | {rexx_fn.name},
                            depth=depth + 1,
                        )
                    rx_svc = self.resolve_callee(fn, "ami_rx_service")
                    if rx_svc and rx_svc.name not in visited_funcs:
                        self.analyze_function(
                            fn=rx_svc,
                            initial_locks=[rexx_wait_lock],
                            call_stack=[
                                *call_stack,
                                f"{fn.name}:ami_netstack_rexx_stop(Wait->ami_rx_main)@{file_line}",
                            ],
                            visited_funcs=visited_funcs | {rx_svc.name},
                            depth=depth + 1,
                        )
                    continue

                callee_fn = self.resolve_callee(fn, ev.name)
                if callee_fn and callee_fn.name not in visited_funcs:
                    if held:
                        self.analyze_function(
                            fn=callee_fn,
                            initial_locks=held,
                            call_stack=[*call_stack, site],
                            visited_funcs=visited_funcs | {callee_fn.name},
                            depth=depth + 1,
                        )

        return held

    def find_cycles(
        self, ignore_locks: Optional[Set[str]] = None
    ) -> List[Tuple[List[str], List[LockEdge]]]:
        ignore = ignore_locks or set()
        adj: Dict[str, Dict[str, LockEdge]] = {}
        for edge in self.edges.values():
            if edge.from_lock in ignore or edge.to_lock in ignore:
                continue
            adj.setdefault(edge.from_lock, {})
            if edge.to_lock not in adj[edge.from_lock]:
                adj[edge.from_lock][edge.to_lock] = edge

        cycles: List[Tuple[List[str], List[LockEdge]]] = []
        seen_canonical: Set[Tuple[str, ...]] = set()

        nodes = sorted(adj.keys())
        for start in nodes:
            stack: List[Tuple[str, List[str], List[LockEdge]]] = [
                (start, [start], [])
            ]
            while stack:
                curr, path, edge_path = stack.pop()
                for nxt, edge in adj.get(curr, {}).items():
                    if nxt == start and len(path) >= 2:
                        min_idx = min(range(len(path)), key=lambda i: path[i])
                        canon = tuple(path[min_idx:] + path[:min_idx])
                        if canon not in seen_canonical:
                            seen_canonical.add(canon)
                            cycles.append(([*path, start], [*edge_path, edge]))
                    elif nxt not in path and nxt > start and len(path) < 7:
                        stack.append((nxt, [*path, nxt], [*edge_path, edge]))
        return cycles


def analyze_source_tree(
    root: Path, src_subdir: str = "src"
) -> Tuple[
    int,
    int,
    LockOrderAnalyzer,
    List[Tuple[List[str], List[LockEdge]]],
]:
    src_dir = root / src_subdir if (root / src_subdir).is_dir() else root
    c_files = sorted(src_dir.rglob("*.c"))
    funcs_by_name: Dict[str, List[FunctionSummary]] = {}
    func_count = 0

    for c_file in c_files:
        rel_path = c_file.relative_to(root).as_posix()
        raw = c_file.read_text(encoding="utf-8", errors="replace")
        clean = strip_comments_strings_and_directives(raw)
        funcs = extract_functions(clean, rel_path)
        for fn in funcs:
            funcs_by_name.setdefault(fn.name, []).append(fn)
            func_count += 1

    analyzer = LockOrderAnalyzer(funcs_by_name)
    analyzer.analyze_all()
    cycles = analyzer.find_cycles()
    return len(c_files), func_count, analyzer, cycles


def run_selftest() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        src = root / "src"
        src.mkdir(parents=True, exist_ok=True)

        # Case 1: Clean lock hierarchy + early return + nested ami_netstack_enter no-op
        (src / "clean.c").write_text(
            """
            void nested_enter_callee(void) {
                AmiNetCaller *c = ami_netstack_enter_alloc();
                ami_netstack_leave_free(c);
            }
            void helper_under_ns(void) {
                ami_ns_lock_obtain();
                ami_ns_lock_release();
            }
            int clean_caller(register struct SocketBase *base __asm("a6")) {
                ObtainSemaphore(&base->sb_Lock);
                if (base->flags == 0) {
                    ReleaseSemaphore(&base->sb_Lock);
                    Forbid();
                    return -1;
                }
                ObtainSemaphore(&bsd_job_lock);
                helper_under_ns();
                ReleaseSemaphore(&bsd_job_lock);
                ReleaseSemaphore(&base->sb_Lock);
                if (bsd_nx_enter(base) == 0) {
                    nested_enter_callee();
                    bsd_nx_leave(base);
                }
                return 0;
            }
            """,
            encoding="utf-8",
        )
        _, _, analyzer, cycles = analyze_source_tree(root)
        if cycles or analyzer.violations:
            print("selftest=FAIL reason=clean_case_flagged", file=sys.stderr)
            return 1

        # Case 2: Lock order cycle (sb_Lock -> ami_ns_lock and ami_ns_lock -> sb_Lock)
        # + lock held across OpenDevice/WaitIO
        # + nx_ip_protection held across ami_sana2_block_enter and tx_semaphore_get
        # + unadopted tx_thread_sleep under ami_ns_lock
        (src / "buggy.c").write_text(
            """
            void path_a(struct SocketBase *base) {
                ObtainSemaphore(&base->sb_Lock);
                ami_ns_lock_obtain();
                ami_ns_lock_release();
                ReleaseSemaphore(&base->sb_Lock);
            }
            void path_b(struct SocketBase *base) {
                ami_ns_lock_obtain();
                ObtainSemaphore(&base->sb_Lock);
                OpenDevice("sana2.device", 0, req, 0);
                tx_thread_sleep(50);
                ReleaseSemaphore(&base->sb_Lock);
                ami_ns_lock_release();
            }
            static void __attribute__((noinline, cold)) ami_sana2_driver_command(
                NX_IP_DRIVER *driver_req, struct netstack_iface *iface) {
                ami_sana2_block_enter();
                Wait(1U);
                ami_sana2_block_leave();
                tx_semaphore_get(&iface->ready, 50);
            }
            void link_down(NX_IP *ip) {
                nx_ip_driver_interface_direct_command(ip, NX_LINK_DISABLE, 0, &status);
            }
            """,
            encoding="utf-8",
        )
        _, _, analyzer2, cycles2 = analyze_source_tree(root)
        has_cycle = any(
            set(c_nodes[:-1]) == {"sb_Lock", "ami_ns_lock"} for c_nodes, _ in cycles2
        )
        has_exec_block = any(
            v.category == "exec_block"
            and v.held_lock == "ami_ns_lock"
            and v.blocking_call == "OpenDevice"
            and v.triage == "unbounded"
            for v in analyzer2.violations.values()
        )
        has_baton_mutex = any(
            v.category == "baton_release_with_mutex"
            and v.held_lock == "nx_ip_protection"
            and v.triage == "unbounded"
            for v in analyzer2.violations.values()
        )
        has_tx_wait = any(
            v.category == "tx_wait"
            and v.held_lock == "nx_ip_protection"
            and v.triage == "unbounded"
            for v in analyzer2.violations.values()
        )
        has_unadopted = any(
            v.category == "unadopted_tx_call"
            and v.held_lock == "ami_ns_lock"
            and v.triage == "unbounded"
            for v in analyzer2.violations.values()
        )
        if not (
            has_cycle
            and has_exec_block
            and has_baton_mutex
            and has_tx_wait
            and has_unadopted
        ):
            print(
                f"selftest=FAIL cycle={has_cycle} exec_block={has_exec_block} "
                f"baton_mutex={has_baton_mutex} tx_wait={has_tx_wait} "
                f"unadopted={has_unadopted}",
                file=sys.stderr,
            )
            return 1

    print("selftest=PASS cases=2")
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Static lock-order and lock-across-blocking-call checker for AmiNetXDuo."
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root containing src/",
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="Run built-in unit self-tests and exit.",
    )
    parser.add_argument(
        "--warn-only",
        action="store_true",
        help="Always exit 0 even when cycles or blocking violations are found.",
    )
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    root = args.root.resolve()
    files_scanned, funcs_scanned, analyzer, cycles = analyze_source_tree(root)

    sorted_edges = sorted(
        analyzer.edges.values(),
        key=lambda e: (e.from_lock, e.to_lock, e.file_line),
    )
    sorted_violations = sorted(
        analyzer.violations.values(),
        key=lambda v: (v.triage, v.category, v.held_lock, v.file_line, v.blocking_call),
    )

    for edge in sorted_edges:
        print(
            f"lock_edge={edge.from_lock}->{edge.to_lock} "
            f"site={edge.file_line} func={edge.func} chain={edge.chain}"
        )

    for idx, (cycle_nodes, cycle_edges) in enumerate(cycles, 1):
        cycle_str = "->".join(cycle_nodes)
        sites_str = ",".join(
            f"{e.from_lock}->{e.to_lock}@{e.file_line}" for e in cycle_edges
        )
        print(f"lock_cycle={cycle_str} index={idx} sites={sites_str}")

    for v in sorted_violations:
        print(
            f"lock_blocking={v.held_lock} kind={v.held_kind} triage={v.triage} "
            f"category={v.category} call={v.blocking_call} site={v.file_line} "
            f"acquired={v.acquired_at} chain={v.chain}"
        )

    baton_mutex_count = sum(
        1 for v in sorted_violations if v.category == "baton_release_with_mutex"
    )
    unbounded_count = sum(1 for v in sorted_violations if v.triage == "unbounded")
    bounded_count = sum(1 for v in sorted_violations if v.triage == "bounded")
    by_design_count = sum(1 for v in sorted_violations if v.triage == "by_design")
    status = "PASS" if (not cycles and unbounded_count == 0) else "FAIL"
    print(f"files_scanned={files_scanned}")
    print(f"functions_scanned={funcs_scanned}")
    print(f"locks_discovered={len(analyzer.locks_discovered)}")
    print(f"edge_count={len(sorted_edges)}")
    print(f"cycle_count={len(cycles)}")
    print(f"blocking_violation_count={len(sorted_violations)}")
    print(f"unbounded_blocking_count={unbounded_count}")
    print(f"bounded_blocking_count={bounded_count}")
    print(f"by_design_blocking_count={by_design_count}")
    print(f"baton_release_with_mutex_count={baton_mutex_count}")
    print(f"status={status}")

    if args.warn_only:
        return 0
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
