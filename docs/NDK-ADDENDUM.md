# Extending the NDK

The NDK's `bsdsocket` interface stopped moving in 2006. What we add past it now
has to reach third-party code, and today it does not: the archive ships **no
headers at all**, and `include/aminetxduo/` is internal. A caller can link
against the 0..143 range because the NDK declares it, and cannot reach anything
we have added since.

This is the plan for closing that. It covers what to ship, and -- more
importantly -- the three places where shipping it means *fixing* an ABI that
nobody else will fix for us.

## What exists to build on

The NDK generates its glue from one file:

```
NDK3.2/SANA+RoadshowTCP-IP/sfd/bsdsocket_lib.sfd
    ==base _SocketBase
    ==bias 30
    LONG socket(LONG domain,LONG type,LONG protocol) (d0,d1,d2)
    ...
```

`sfdc` turns that into `proto/bsdsocket.h`, `inline/bsdsocket.h` and
`pragmas/bsdsocket_pragmas.h`. The inline form is a statement-expression macro
that loads the argument registers and jumps through `SocketBase` at the LVO.

LVO arithmetic, so the next person does not have to rederive it: entry *n* of
the SFD is at `-(30 + 6n)`. Our vector array carries the four standard library
vectors (Open/Close/Expunge/Reserved) at `-6..-24` before `socket`, so

```
array index = SFD entry + 4
```

The array ends at `[145]` = `-0x36c`. The next free slot is `[146]` = `-0x372`.

## Three decisions we are making for everyone

These are the reason this needs planning rather than just doing.

**1. LVO slots.** `[146]` onward. Do **not** take `[141]`/`[142]`: they are
`bsd_enosys` *reserved* inside AmiTCP's own published range, and a future
Roadshow may define them. Once we hand out `-0x372` for `if_nametoindex`, it is
that forever.

**2. `CMSG_ALIGN` for m68k.** `struct cmsghdr` and the `CMSG_*` macros are not
in the NDK. RFC 3542 leaves the alignment to the implementation, so we pick it,
and every ancillary-data buffer any caller ever builds depends on the choice.
Pick it once, write down why, and never move it.

**3. `IPV6_*` option numbers.** Also not in the NDK. They must not collide with
anything Roadshow might assign. Same permanence.

## Shape of the addendum

A `Developer/` drawer in the archive:

```
Developer/
    include/aminetxduo/     the new types and prototypes
    sfd/aminetxduo_lib.sfd  our additions only
    inline/  proto/  pragmas/    generated from that sfd
```

**A separate SFD, not a fork of Commodore's.** A fork has to be re-merged every
time the NDK moves and silently diverges when it is not. A small separate file
declaring the same `==base _SocketBase` generates its own inline header, and a
caller including both reaches all of it -- the base variable is shared, so the
two sets mix with no glue.

**Additive header paths, not shadowed ones.** RFC 3493 says `if_nametoindex()`
comes from `<net/if.h>`, and a developer porting Unix code will expect that. But
shipping a `net/if.h` means include order decides whether ours or the NDK's
wins, silently, per translation unit. So: the headers live under
`aminetxduo/` and never replace an NDK header. If the standard spellings turn
out to matter, an optional overlay drawer can be documented as
put-this-first-and-here-is-the-risk -- opt-in, not the default.

**Runtime detection is `lib_Revision`.** A header cannot tell a caller whether
the library actually in memory has the vectors; only the revision can, which is
the mechanism `tool_netstatus_open()` already uses and which we bumped to 2 on
2026-07-31. Every addendum symbol carries the revision that introduced it, and
the drawer ships the check as a documented three-liner rather than leaving each
caller to invent one.

## Contents, in dependency order

1. **RFC 3493 §4** -- `struct if_nameindex`, `IF_NAMESIZE`, and the four
   prototypes. New LVOs `[146]`..`[149]`. Indices are 1-based, matching
   `rtm_index`; change neither alone.
2. **RFC 3542** -- `struct cmsghdr` + `CMSG_*`, `struct in6_pktinfo`,
   `struct icmp6_filter` + its six macros, and the `IPV6_*` option numbers. No
   new LVOs: it rides `sendmsg`/`recvmsg`, and `struct msghdr` is already the
   28-byte 4.4BSD shape with `msg_control` at offset 16.
3. **What is already ours and private** -- `netstatus.h`'s `NetStackQuery` /
   `NetStackControl` at `-0x366`/`-0x36c`. Publishing these is what lets someone
   else write a `netstat`. Decide deliberately: published means frozen.

Order matters because (1) sets the slot precedent and (2) sets the alignment
precedent, and (3) is the one that is purely a decision to publish what already
works.

## What this does not cover

RFC 4007's `%zone` text form is behaviour, not header -- it needs a parser and a
formatter, and no ABI. It is in `BACKLOG.md` on its own.
