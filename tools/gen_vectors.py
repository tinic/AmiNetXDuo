#!/usr/bin/env python3
"""Generate the bsdsocket.library LVO vector table.

The authoritative sources are the freely distributable Roadshow NDK headers
that ship with the local m68k toolchain:

  pragmas/bsdsocket_pragmas.h   one `#pragma amicall(SocketBase,0xNNN,name(regs))`
                                per vector -- byte offset + register assignment
  clib/bsdsocket_protos.h       the prototype for each of those names

Nothing here is hand-maintained except IMPLEMENTED (below), which says which
vectors have a real implementation in src/bsdsocket and which get the shared
ENOSYS stub.  The table is *dense*: every slot from the first user vector
(0x01e) to the last one the ABI defines points at executable code, because a
NULL in an LVO slot is a guru on the first call.

Output (both checked in, regenerate with `python3 tools/gen_vectors.py`):

  src/bsdsocket/bsdsocket_vectors.h   extern declarations, register-annotated
  src/bsdsocket/bsdsocket_vectors.c   the table itself

Usage:
  tools/gen_vectors.py [--ndk DIR] [--outdir DIR] [--check]

SPDX-License-Identifier: MIT
"""

import argparse
import os
import re
import sys

DEFAULT_NDK = os.path.expanduser(
    "~/amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include")

# The first four LVOs (-6 Open, -12 Close, -18 Expunge, -24 Reserved) are the
# exec standard set; user vectors start at -30 == -0x01e.
FIRST_USER_OFFSET = 0x01e
VECTOR_STRIDE = 6

# Trailing reserved slots.  clib/bsdsocket_protos.h documents "six reserved
# slots for future expansion" after getnameinfo(); we emit stubs for them so a
# caller built against a later Roadshow gets ENOSYS instead of a jump into the
# library's data.
TRAILING_RESERVED = 6

# ---------------------------------------------------------------------------
# Vectors that are OURS, past the end of everything the published ABI names.
#
# (offset, C symbol, guard macro).  Each is emitted inside `#ifdef <guard>`, in
# both the table and the header, so a build without the guard has neither the
# slot nor the code -- which is what lets the default bsdsocket.library stay
# byte-identical to a build of a tree that has no TLS in it at all.
#
# 0x360 is the first slot after the six reserved ones clib/bsdsocket_protos.h
# documents following getnameinfo(), i.e. after every offset any published
# bsdsocket ABI assigns.  Callers must still present a magic and a version
# (include/aminetxduo/nxcontext.h) so that a program aiming at some future
# vendor's vector at the same offset gets a clean failure rather than a
# pointer.
# ---------------------------------------------------------------------------
PRIVATE_VECTORS = [
    (0x360, "bsd_ObtainNetXDuoContext", "AMINETXDUO_TLS_CONTEXT",
     "hands tls.library the NetX Duo singleton -- nxcontext.h"),
]

# Hand-written because these have no NDK pragma to read a register assignment
# out of -- they are ours, so the assignment is ours to state.
PRIVATE_DECLARATIONS = {
    "bsd_ObtainNetXDuoContext": """\
LONG bsd_ObtainNetXDuoContext(
        register ULONG                    magic       __asm("d0"),
        register ULONG                    version     __asm("d1"),
        register const AmiNetXDuoContext **ctx        __asm("a0"),
        register struct AmiSocketBase    *SocketBase  __asm("a6"));""",
}

# ---------------------------------------------------------------------------
# What is actually implemented.  Everything else gets bsd_enosys().
#
# Tier 1 per docs/RESEARCH.md S3.2: socket core, data transfer, WaitSelect,
# IoctlSocket, the errno family, SocketBaseTagList, the inet_* conversions and
# the resolver entry points.  GetSocketEvents/SetSocketSignals/Dup2Socket come
# along for free because the event plumbing and the descriptor table are here.
# ---------------------------------------------------------------------------
IMPLEMENTED = {
    # socket core
    "socket", "bind", "listen", "accept", "connect", "shutdown", "CloseSocket",
    # data transfer
    "send", "sendto", "recv", "recvfrom", "sendmsg", "recvmsg",
    # options / ioctl / names
    "setsockopt", "getsockopt", "getsockname", "getpeername", "IoctlSocket",
    "getdtablesize", "Dup2Socket",
    # descriptor hand-off between tasks (handoff.c)
    "ObtainSocket", "ReleaseSocket", "ReleaseCopyOfSocket",
    "ObtainServerSocket", "ProcessIsServer",
    # multiplexing and the event API
    "WaitSelect", "SetSocketSignals", "GetSocketEvents",
    # the raw packet path (bpf.c -> src/bpf/).  Always in the table: when
    # AMINETXDUO_BPF is off, bpf.c compiles to eight ENOSYS bodies rather than
    # to nothing, so the slot shape never depends on a build option.
    "bpf_open", "bpf_close", "bpf_read", "bpf_write",
    "bpf_set_notify_mask", "bpf_set_interrupt_mask",
    "bpf_ioctl", "bpf_data_waiting",
    # errno / tags
    "Errno", "SetErrnoPtr", "SocketBaseTagList",
    # address conversion
    "inet_addr", "inet_aton", "inet_network", "inet_ntop", "inet_pton",
    "Inet_NtoA", "Inet_LnaOf", "Inet_NetOf", "Inet_MakeAddr",
    "In_LocalAddr", "In_CanForward",
    # resolver
    "gethostbyname", "gethostbyaddr", "gethostbyname_r", "gethostbyaddr_r",
    "gethostname", "gethostid",
    # the DEVS:Internet netdb (netdb.c)
    "getnetbyname", "getnetbyaddr", "getservbyname", "getservbyport",
    "getprotobyname", "getprotobynumber",
    "setnetent", "endnetent", "getnetent",
    "setprotoent", "endprotoent", "getprotoent",
    "setservent", "endservent", "getservent",
    # the family-agnostic resolver (addrinfo.c).  Present in both build
    # configurations; only the AF_INET6 answers depend on AMINETXDUO_IPV6.
    "getaddrinfo", "getnameinfo", "freeaddrinfo", "gai_strerror",
    # Tier 3, read-only query side (roadshow.c).
    #
    # Only the calls whose contract is FULLY specified by the headers that
    # ship with the toolchain.  There is no bsdsocket.doc autodoc anywhere on
    # this machine (docs/RESEARCH.md S3.2), so for the rest of Tier 3 -- the
    # interface list, QueryInterfaceTagList, GetNetworkStatistics, the routing
    # calls, ObtainRoadshowData -- the *data structures* are either undefined
    # in the NDK or defined without the naming/return conventions that make
    # them usable.  Handing a stock Roadshow tool a list of the wrong node
    # shape is worse than ENOSYS: it gurus inside the application.  Those stay
    # stubbed until a primary source turns up.
    "ObtainDomainNameServerList", "ReleaseDomainNameServerList",
    "GetDefaultDomainName",
}

# Return types that are pointers without saying so with a '*'.  A stub for one
# of these has to return NULL, not -1 (see bsd_enosys_ptr in errno.c).
POINTER_TYPEDEFS = {"STRPTR", "CONST_STRPTR", "APTR", "BPTR", "CONST_APTR"}


def returns_pointer(ret):
    return ret is not None and (ret.endswith("*") or ret in POINTER_TYPEDEFS)


def returns_bool(ret):
    """A stub for one of these must return FALSE, not -1.

    -1 is all-bits-set, which every BOOL test in the world reads as TRUE.  A
    -1 stub for ProcessIsServer(), GetDefaultDomainName() or
    ChangeRoadshowData() therefore tells the caller the operation SUCCEEDED,
    which is the one thing an unimplemented vector must never do.
    """
    return ret == "BOOL"


AMICALL_RE = re.compile(
    r"#pragma\s+amicall\s*\(\s*SocketBase\s*,\s*0x([0-9a-fA-F]+)\s*,\s*"
    r"(\w+)\s*\(([^)]*)\)\s*\)")

PROTO_RE = re.compile(r"^__stdargs\s+(.+?)\s*\b(\w+)\s*\(\s*(.*?)\s*\)\s*;\s*$")


def parse_pragmas(path):
    """[(offset, name, [register, ...]), ...] in file order."""
    out = []
    with open(path, "r", encoding="latin-1") as fh:
        for line in fh:
            m = AMICALL_RE.search(line)
            if not m:
                continue
            offset = int(m.group(1), 16)
            name = m.group(2)
            regs = [r.strip() for r in m.group(3).split(",") if r.strip()]
            out.append((offset, name, regs))
            # The header repeats the whole table for other compilers; the
            # amicall block is the only one we parse, and it comes first.
            if name == "getnameinfo":
                break
    return out


def split_args(text):
    """Split a parameter list on top-level commas."""
    args, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    return args


def parse_arg(arg, index):
    """('struct sockaddr *', 'name') from 'struct sockaddr *name'."""
    arg = arg.replace("__timeval", "timeval").strip()
    m = re.match(r"^(.*?)(\**)\s*(\w+)$", arg)
    if not m:
        # Unnamed parameter -- invent one.
        return arg, "a%d" % index
    base, stars, name = m.group(1).strip(), m.group(2), m.group(3)
    if not base:            # e.g. a bare "VOID"
        return name, "a%d" % index
    ctype = base + (" " + stars if stars else "")
    name = name.lstrip("_") or ("a%d" % index)
    return ctype.strip(), name


def parse_protos(path):
    """{name: (return_type, [(ctype, argname), ...])}"""
    out = {}
    with open(path, "r", encoding="latin-1") as fh:
        for line in fh:
            line = line.strip()
            m = PROTO_RE.match(line)
            if not m:
                continue
            ret, name, argtext = m.group(1).strip(), m.group(2), m.group(3)
            if ret.endswith("*"):
                ret = ret[:-1].strip() + " *"
            argtext = argtext.strip()
            if argtext in ("", "VOID", "void"):
                args = []
            else:
                args = [parse_arg(a, i) for i, a in enumerate(split_args(argtext))]
            out[name] = (ret, args)
    return out


def declaration(name, ret, args, regs):
    """One register-annotated extern declaration."""
    params = []
    for (ctype, argname), reg in zip(args, regs):
        sep = "" if ctype.endswith("*") else " "
        params.append('register %s%s%s __asm("%s")' % (ctype, sep, argname, reg))
    params.append('register struct AmiSocketBase *SocketBase __asm("a6")')
    sep = "" if ret.endswith("*") else " "
    lead = "%s%sbsd_%s(" % (ret, sep, name)
    indent = " " * len(lead)
    body = (",\n" + indent).join(params)
    return "%s%s);" % (lead, body)


def generate(vectors, protos):
    by_offset = {}
    for offset, name, regs in vectors:
        if name not in protos:
            # Vectors the NDK assigns an offset to but does not prototype:
            # ChangeRouteTagList and the ipf_* packet-filter set (Roadshow
            # private, out of scope per docs/RESEARCH.md S9). They still need
            # a dense slot, so record them with no signature -- always stubbed.
            if name in IMPLEMENTED:
                raise SystemExit("no prototype for implemented vector %s" % name)
            by_offset[offset] = (name, None, None, regs)
            continue
        ret, args = protos[name]
        if len(args) != len(regs):
            raise SystemExit(
                "%s: %d prototype arguments but %d registers (%s)"
                % (name, len(args), len(regs), ",".join(regs)))
        by_offset[offset] = (name, ret, args, regs)
    return by_offset


HEADER_PREAMBLE = """\
/*
 * bsdsocket.library -- LVO vector declarations.
 *
 * GENERATED by tools/gen_vectors.py from the Roadshow NDK headers
 * (pragmas/bsdsocket_pragmas.h + clib/bsdsocket_protos.h).  Do not edit;
 * re-run the generator instead.
 *
 * Every vector is declared with the exact m68k register assignment the ABI
 * specifies, plus the library base in a6.  Implementations in src/bsdsocket
 * include this header, so the compiler checks each definition against the
 * ABI rather than against a hand-copied signature.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_VECTORS_H
#define AMINETXDUO_BSDSOCKET_VECTORS_H

#include "bsdsocket_internal.h"

#include <sys/mbuf.h>
#include <net/route.h>

/* The private vector below traffics in one of these. */
#ifdef AMINETXDUO_TLS_CONTEXT
#include "aminetxduo/nxcontext.h"
#endif

/* The shared stubs every unimplemented slot points at: they set errno to
 * ENOSYS and return the "failed" value for their shape.  Never NULL in the
 * table -- a jump through a NULL LVO takes the machine down, and Tier-3
 * vectors do get called by stock Roadshow tools.
 *
 * Three of them because the shapes differ.  A vector documented to return a
 * pointer must return NULL on failure, not -1: its callers test for NULL and
 * then dereference, so a -1 stub turns "unimplemented" into a guru inside the
 * application.  A vector documented to return BOOL must return FALSE, because
 * -1 is all-bits-set and every BOOL test reads that as TRUE -- a -1 stub for
 * ProcessIsServer() or ChangeRoadshowData() reports SUCCESS. */
LONG bsd_enosys(register struct AmiSocketBase *SocketBase __asm("a6"));
APTR bsd_enosys_ptr(register struct AmiSocketBase *SocketBase __asm("a6"));
BOOL bsd_enosys_bool(register struct AmiSocketBase *SocketBase __asm("a6"));

"""

HEADER_EPILOGUE = """
/* The vector table itself, terminated by (APTR)-1, as MakeLibrary wants it. */
extern const APTR BsdVectorTable[];

#endif /* AMINETXDUO_BSDSOCKET_VECTORS_H */
"""

SOURCE_PREAMBLE = """\
/*
 * bsdsocket.library -- the LVO vector table.
 *
 * GENERATED by tools/gen_vectors.py.  Do not edit.
 *
 * MakeLibrary() lays these out at 6 bytes per entry going down from the
 * library base, so entry i sits at LVO -(6 * (i + 1)).  The first four are
 * the exec standard set; user vector n is at index (offset / 6) - 1.
 *
 * The table is dense on purpose.  Slots the ABI reserves (0x132..0x168 and
 * the two after gethostbyaddr_r) and every vector we have not implemented
 * yet point at bsd_enosys() rather than NULL.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

/* Function pointers of assorted shapes go into one table; exec only ever
 * copies them into JMP instructions, so the C type is irrelevant. */
typedef VOID (*BsdVector)(VOID);

const APTR BsdVectorTable[] =
{
"""


def emit(by_offset, outdir, check=False):
    offsets = sorted(by_offset)
    last = offsets[-1] + TRAILING_RESERVED * VECTOR_STRIDE

    impl = [n for n in IMPLEMENTED]
    unknown_impl = [n for n in impl
                    if n not in {v[0] for v in by_offset.values()}]
    if unknown_impl:
        raise SystemExit("IMPLEMENTED names not in the ABI table: %s"
                         % ", ".join(sorted(unknown_impl)))

    # ---------------------------------------------------------------- header
    h = [HEADER_PREAMBLE]
    for offset in offsets:
        name, ret, args, regs = by_offset[offset]
        if name not in IMPLEMENTED:
            continue
        h.append("/* LVO -0x%03x */\n%s\n\n"
                 % (offset, declaration(name, ret, args, regs)))
    for offset, symbol, guard, what in PRIVATE_VECTORS:
        h.append("/* LVO -0x%03x -- PRIVATE: %s */\n#ifdef %s\n%s\n#endif\n\n"
                 % (offset, what, guard, PRIVATE_DECLARATIONS[symbol]))
    h.append(HEADER_EPILOGUE)
    header = "".join(h)

    # ---------------------------------------------------------------- source
    s = [SOURCE_PREAMBLE]
    s.append("    /* -6 Open, -12 Close, -18 Expunge, -24 Reserved */\n")
    for entry in ("bsd_lib_open", "bsd_lib_close", "bsd_lib_expunge",
                  "bsd_lib_reserved"):
        s.append("    (APTR)%s,\n" % entry)
    s.append("\n")

    implemented = stubbed = stubbed_ptr = stubbed_bool = 0
    for offset in range(FIRST_USER_OFFSET, last + 1, VECTOR_STRIDE):
        entry = by_offset.get(offset)
        index = offset // VECTOR_STRIDE - 1
        if entry is None:
            s.append("    (APTR)bsd_enosys,%s/* -0x%03x [%3d] reserved */\n"
                     % (" " * 20, offset, index))
            stubbed += 1
            continue
        name, ret = entry[0], entry[1]
        if name in IMPLEMENTED:
            target = "bsd_%s" % name
            implemented += 1
        elif returns_pointer(ret):
            # Documented to return a pointer, so the stub must return NULL.
            target = "bsd_enosys_ptr"
            stubbed += 1
            stubbed_ptr += 1
        elif returns_bool(ret):
            # Documented to return BOOL, so the stub must return FALSE.
            target = "bsd_enosys_bool"
            stubbed += 1
            stubbed_bool += 1
        else:
            target = "bsd_enosys"
            stubbed += 1
        pad = max(1, 34 - len("    (APTR)%s," % target))
        s.append("    (APTR)%s,%s/* -0x%03x [%3d] %s */\n"
                 % (target, " " * pad, offset, index, name))

    for offset, symbol, guard, what in PRIVATE_VECTORS:
        index = offset // VECTOR_STRIDE - 1
        s.append("\n#ifdef %s\n" % guard)
        s.append("    /* -0x%03x [%3d] %s -- PRIVATE: %s */\n"
                 % (offset, index, symbol, what))
        s.append("    (APTR)%s,\n" % symbol)
        s.append("#endif\n")

    s.append("\n    (APTR)-1\n};\n")
    source = "".join(s)

    named = len(by_offset)
    total = (last - FIRST_USER_OFFSET) // VECTOR_STRIDE + 1
    summary = ("%d ABI vectors (%d named, %d reserved/private), "
               "%d implemented, %d stubbed "
               "(%d of them NULL-returning, %d FALSE-returning)"
               % (total, named, total - named, implemented, stubbed,
                  stubbed_ptr, stubbed_bool))
    source = source.replace(" * SPDX-License-Identifier: MIT",
                            " * Coverage: %s.\n *\n * SPDX-License-Identifier: MIT"
                            % summary, 1)

    paths = [(os.path.join(outdir, "bsdsocket_vectors.h"), header),
             (os.path.join(outdir, "bsdsocket_vectors.c"), source)]
    changed = False
    for path, text in paths:
        old = None
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as fh:
                old = fh.read()
        if old == text:
            continue
        changed = True
        if check:
            print("out of date: %s" % path, file=sys.stderr)
            continue
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        print("wrote %s" % path)

    print(summary)
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ndk", default=DEFAULT_NDK,
                    help="NDK include directory (default: %(default)s)")
    ap.add_argument("--outdir", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "src", "bsdsocket"))
    ap.add_argument("--check", action="store_true",
                    help="fail if the checked-in files are stale")
    args = ap.parse_args()

    pragmas = os.path.join(args.ndk, "pragmas", "bsdsocket_pragmas.h")
    protos = os.path.join(args.ndk, "clib", "bsdsocket_protos.h")
    for path in (pragmas, protos):
        if not os.path.exists(path):
            raise SystemExit("missing NDK header: %s" % path)

    table = generate(parse_pragmas(pragmas), parse_protos(protos))
    changed = emit(table, args.outdir, check=args.check)
    if args.check and changed:
        raise SystemExit("bsdsocket_vectors.[ch] are out of date")


if __name__ == "__main__":
    main()
