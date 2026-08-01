# Alignment and stack

Two hazards that do not exist on any machine this was written on, audited the
way `docs/ALLOCATIONS.md` audits `ami_alloc()`: every site, the verdict, and the
interesting ones explained.

**`grep` needs `LC_ALL=C grep -a`** in this tree -- the NDK headers are Latin-1
and a plain `grep` reads them as binary and silently returns nothing.

---

# Part 1 -- unaligned access

A 68000 or 68010 takes an **Address Error** (exception 3) on a word or longword
access to an odd address. Not a slow path: a Guru. The 68020 and later tolerate
it, so this fails only on the machines least able to report why -- and a bare
68000 with 1 MB and OS 2.04 is the measured floor here, not an aspiration.

## The rule that shrinks the problem

`__alignof__(long)` on m68k is **2**, not 4. Measured, not assumed:

```
$ m68k-amigaos-gcc -m68000 -Os -S
__alignof__(long)              2
__alignof__(struct cmsghdr)    2
sizeof(struct { char c; long l; })   6
```

Everything follows from that. GCC never places a `long` at an odd offset in a
struct, `AllocMem` is 8-byte aligned, `AllocVec` returns that minus a longword
(measured: 4-aligned, never 8), and the 68000 keeps SP even in hardware. **So no
compiler-placed or Exec-allocated object in this tree is ever at an odd
address.** An Address Error here can only come from a pointer *we* computed --
byte arithmetic over wire data, a carve out of a caller's buffer, or a cast of
something a caller handed us.

That is the search, and it is much smaller than the grep suggests.
`tools/smoke/alignprobe` prints all of the above from a running machine so the
claim is checkable rather than argued; see "Proof on a 68000" below.

## The packet path -- clean, and now asserted

Every longword NetX Duo reads out of an IP, TCP or UDP header, and every
longword `src/net68k/n68k_checksum.c` sums, depends on one arithmetic identity:

| | |
|---|---|
| `nx_packet_data_start` | multiple of `NX_PACKET_ALIGNMENT`, which is `sizeof(ULONG)` = 4. `_nx_packet_pool_create()` rounds both the pool base and the payload address up to it (`nx_packet_pool_create.c:90/105`) |
| `AMI_SANA2_RX_PAD` | 2 (`sana2_internal.h:118`) |
| `AMI_ETH_HEADER_SIZE` | 14 (`aminetxduo/sana2.h:44`) |
| IP header lands at | `data_start + 2 + 14` = `data_start + 16` |

Both receive modes agree. Cooked: the device writes at `base + 14` and the
14-byte header is synthesised in front of it. Raw: the device writes the whole
frame at `base`, and `ami_sana2_rx_deliver()` steps the prepend pointer past 14
bytes (`sana2_rx.c:89`). Either way the IP header is at `data_start + 16`.
Transmit is the same identity from the other end: `NX_PHYSICAL_HEADER` is 16,
deliberately, and the driver's own header build is byte-wise
(`sana2_tx.c:392-400`).

This was prose in `sana2_internal.h` and nothing checked it. It is now a
`_Static_assert` in `sana2_rx.c`, because `n68k_checksum.c` does not check it
either -- `long_ptr = (ULONG *)prepend_ptr` at `:156` and `:180`, and the
end-pointer rounding at `:189` is only correct if that pointer is 4-aligned. The
file's own comment says "always, here". It is, and now it cannot quietly stop
being.

Non-Ethernet SANA-II devices do not break it: raw mode is only probed for
`S2WireType_Ethernet` (`sana2_device.c:300`), and cooked mode synthesises a
14-byte header whatever the device's address size is.

## Every wide access over data we do not own

| Site | What the pointer is | Verdict |
|---|---|---|
| `net68k/n68k_checksum.c:156/180/204` | `nx_packet_prepend_ptr` as `ULONG *`, summed by `n68k_sum_longwords()` | clean -- 4-aligned by the identity above, now asserted |
| `net68k/n68k_checksum.c:216/234/248` | the same pointer as `USHORT *` | clean -- guarded by `& 3 == 2` at `:214`, and word access needs only even |
| `net68k/n68k_copy.c:39/60-71` | arbitrary caller buffers | clean -- bails to a byte loop when `to` and `from` disagree in bit 0, aligns `to` before the longword path. The one place in the tree already hardened for this |
| `bpf/bpf_channel.c:44-68` `ami_bpf_copy_bytes` | capture records and packet payload | clean -- same shape as `n68k_copy` |
| `bpf/bpf_channel.c:563/564` record walk | `ch->hold + pos + 8`, an odd-ish offset by construction | clean -- `ami_bpf_get32`/`get16` compose the value from bytes. The whole record ABI is byte-wise on purpose (`bpf_channel.c:87`) |
| `bpf/bpf_filter.c:91-134` | the captured frame, at BPF-controlled offsets | clean -- `value = (value << 8) \| p[i]`. The one place you would most expect this bug and it is not there |
| `bpf/bpf_channel.c:874-1035` ioctl | the **caller's** `APTR`, read/written as `ULONG`, `UWORD` and three struct overlays | **was unguarded -- fixed** |
| `bsdsocket/cmsg.c:474` parse loop | `msg_control + at`, `at` stepping by `CMSG_ALIGN` | clean -- multiples of 4, and the base is gated |
| `bsdsocket/cmsg.c:336/468` the gates | the caller's `msg_control` | **were `& 3`, which refused half of all valid buffers -- fixed** |
| `include/aminetxduo/cmsg.h` `CMSG_BUFFER()` | the macro that exists to make a control buffer aligned | **did not -- fixed** |
| `bsdsocket/resolver.c:112-117` | `char **` and `ULONG *` carved out of the caller's `buf` | **was unchecked -- fixed** |
| `bsdsocket/addralloc.c:319-403` | seven buffers carved out of one `ami_alloc` block | clean -- every step is `bsd_aam_round()` to 4, and `tests/tools/run-ifquery.sh:616` asserts the result on a running machine |
| `mbuf/mbuf_alloc.c:211-223` | `struct mbuf *` at `base + i * MSIZE` | clean -- `base` is stepped up to `MSIZE`, which is a power of two ≥ 4 |
| `bsdsocket/select.c:581/592` | the caller's `fd_set` as `ULONG *` | clean -- `fd_set` is an array of `long`, so 2-aligned at worst, and a longword load needs only even |
| `bsdsocket/errno.c:72-73`, `usergroup/ug_context.c:31-32` | the application's `errno`, through `SBTC_ERRNOPTR` | clean by contract -- the size is the caller's declaration and the object is a C `int`. Roadshow does the same |
| `bsdsocket/errno.c:528/539/656` | `ti_Data` as a `ULONG *` | clean -- a by-reference tag points at a caller's `ULONG` |
| `bsdsocket/errno.c:554/753` | `(ULONG *)((UBYTE *)base + offset)` | clean -- the offsets are `offsetof` of `ULONG` members |
| `bsdsocket/options.c`, `mcast.c`, `in6.c`, `cmsg.c:671-706` | `optval` as `LONG *`/`WORD *` | clean by contract -- a `setsockopt` value is a C object of that type |
| `bsdsocket/socket.c:1188` | the caller's `sockaddr_in` | clean -- `s_addr` is at offset 4 of a C struct |
| `bsdsocket/socket.c:1258` | writing the caller's `sockaddr` | clean -- `bsd_bcopy` out of a local, byte-wise, whatever the caller's alignment |
| `src/tools/*` `*(LONG *)args[N]` | ReadArgs's own storage | clean -- `ReadArgs` allocates it |
| `src/tools/nettrace.c:373-378` | the capture buffer it read back | clean -- `nt_get32`/`nt_get16`, byte-wise, and the pcap writer is byte-wise too |
| `src/config/*` | file text | clean -- byte-wise throughout, no wide cast anywhere in the directory |
| `src/netstack/*` | DHCP options, DNS answers | clean -- shift-and-OR byte composition (`netstack.c:1993`, `netstack_dns.c:105`) |
| `sana2/sana2_rx.c:269/385`, `sana2_tx.c:200` | `struct Message *` from `GetMsg()` back to a slot | clean -- the slot lives in an `ami_alloc`ed struct |
| `port/netxduo-amiga/inc/nx_port.h` | `NX_CHANGE_USHORT_ENDIAN` at `nxd_mdns.c:8489`, `*(USHORT *)(prepend + 2)` | clean -- offset 2 from a 4-aligned prepend is even |
| vendored NetX Duo, whole tree | `(ULONG *)ptr + N` in `nx_tcp_packet_send_control.c:252` and three others | clean -- element arithmetic, not byte arithmetic. No wide cast at an odd byte offset exists in the vendored source |

Nineteen categories, five defects, five fixed.

### `CMSG_BUFFER()` did not align anything (FIXED)

The header says it plainly: *"a control buffer has to be aligned for a
socklen_t and the usual `char buf[CMSG_SPACE(n)]` is not, so the union every
portable program writes by hand is written once here."* The union was

```c
union { struct cmsghdr cmsgbuf_align; UBYTE cmsgbuf_bytes[bytes]; }
```

and on m68k `__alignof__(struct cmsghdr)` is 2, so the union's alignment is 2.
Even. Not 4. Half of every `CMSG_BUFFER` a program declares lands 2 mod 4.

`bsd_cmsg_build()` and `bsd_cmsg_parse()` then tested `& 3` and refused exactly
those: `MSG_CTRUNC` with nothing written on receive, `EINVAL` on send. So the
library refused its own documented idiom, about half the time, depending on
where the linker happened to put the buffer -- and passed its own tests, because
`tests/ipv6/ipv6_socket_test.c` declares its three buffers at file scope where
they happened to land on 4.

Both halves were wrong and both are fixed. The union now carries
`__attribute__((aligned(4)))` -- nothing in the language reaches 4 on this
target, so the attribute is the mechanism and not a hint -- and `cmsg.c` pins it
with a `_Static_assert` over a `typedef CMSG_BUFFER(...)`. The runtime gates drop
from `& 3` to `& 1`, which is what the hardware requires and what the header's
own prose already said ("an odd `msg_control` is not faulted on either way").
A buffer 2 mod 4 is a perfectly good place to read a longword on a 68000, and
refusing one was a restriction with nothing behind it.

Nothing downstream of the gate needs more than even: `at` steps by
`CMSG_ALIGN` (4), so every `struct cmsghdr` in the walk keeps the base's parity;
`CMSG_DATA` is `+12`; `in6_pktinfo`'s `ipi6_ifindex` is at `+16` and
`in_pktinfo`'s three fields at 0, 4 and 8.

### `bsd_hostent_pack()` trusted the caller's buffer (FIXED)

`resolver.c:73` carves the `_r` reply out of the application's `buf`:

```
char *addr_list[2] | char *aliases[1] | ULONG address | name
```

with a comment reading *"all longword aligned, which is what m68k needs"*. That
was true of the offsets and false of the pointers: `buf` is an `APTR` from an
application and nothing established or checked its alignment, so an odd one is
an Address Error at `*address = BSD_HTONL(addr)` -- the first store, before any
of the three pointer writes. Reachable from `gethostbyname_r` and
`gethostbyaddr_r`, both of which are in the LVO table.

Fixed by stepping the carve base up to a longword boundary and counting the
slack against `buflen`, so a short buffer is still `ERANGE` rather than an
overrun. Rounding rather than refusing: the caller did nothing wrong by C's
rules and has no way to know what we wanted.

### `ami_bpf_ioctl()` dereferenced the caller's `APTR` (FIXED)

Ten of its commands read or write a `ULONG` or `UWORD` through `buffer`
(`bpf_channel.c:874`, `:881`, `:896`, `:903`, `:934`, `:997`, `:1004`,
`:1013`, `:1024`, `:1032`), and three of those go through a struct overlay
(`bpf_stat`, `bpf_version`, `bpf_program`). `buffer` arrives from an application
across the `bpf_ioctl` LVO, so it is whatever was passed.

Not reachable from conforming C -- see the rule at the top: a caller's `ULONG`
is at an even address whatever the compiler does with it -- but the LVO is
public and the guard is three lines. An odd buffer is now `EINVAL`. The three
`ifreq` commands are exempt (`BIOCGETIF`, `BIOCSETIF`, `SIOCGIFADDR`): they
touch bytes only, including the `sockaddr_in` that `SIOCGIFADDR` writes out by
hand, so refusing them would repeat the `& 3` mistake in a different file.

### The packet-alignment identity was prose (FIXED)

`_Static_assert`s in `sana2_rx.c`, as above.

## Proof on a 68000

`tools/smoke/alignprobe` reports, from a running machine: `__alignof__` for a
`long`, a `short`, a pointer and a `struct cmsghdr`; the alignment
`CMSG_BUFFER()` delivers on the stack, in static storage and as a struct
member; what `AllocVec()` and `AllocMem()` return for three sizes; and a
longword load from an address 2 mod 4. It does not provoke an Address Error --
that is a Guru, not a test result, and whether the hazard is real was never the
question.

```
cmake -B build/m0 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
      -DAMINETXDUO_CPU=68000
cmake --build build/m0 --target smoke_alignprobe
. ~/amiga-assets/env.sh
export AMINETXDUO_KICKSTART_A600=".../Kickstart v3.1 r40.63 (A500-A600-A2000).rom"
tools/amiberry-run.sh -m A600 -t 90 build/m0/tools/smoke/alignprobe
```

**`-m A600`.** An A1200 is a 68020 and tolerates everything this is about; a
pass there proves nothing. The binary must come from a `-DAMINETXDUO_CPU=68000`
build or the machine stops on an illegal instruction before `main` runs, and
the A600 needs its own ROM -- 40.63 or 37.350. The A1200's 40.68 boots to a
black screen with nothing on the serial port to say why.

Run on an A600, Kickstart 40.63, from a `-DAMINETXDUO_CPU=68000` build, once
with the fix and once with only `CMSG_BUFFER_ALIGN4` emptied out:

```
                                          before the fix     after
ALIGN alignof long=2 short=2 ptr=2 cmsghdr=2               (both)
ALIGN sizeof(struct{char;long;})=6                         (both)
ALIGN cmsgbuf offset in a struct =              2              4
ALIGN CMSG_BUFFER behind a UWORD             FAIL             ok
ALIGN allocvec 00203164 002182A4 0021A674  ok  ok  ok  (both)
ALIGN allocmem 00218028                                    (both)
ALIGN longword load from an address 2 mod 4  ok             ok
                                        ALIGNPROBE FAIL 1  PASS
```

Three things worth keeping out of that:

- **`__alignof__(long)` really is 2 on the machine**, not only in the
  compiler. Everything in Part 1 rests on it.
- **The struct case is the one that answers the same way every run.** The stack
  local passed both times: where the frame puts it is a coin flip, which is
  exactly why the bug survived. A `CMSG_BUFFER` behind a `UWORD` in a struct is
  at offset 2 with an alignment of 2 and at offset 4 with an alignment of 4, in
  every instance, and that is the case the probe now turns on.
- **`AllocVec()` returns 4-byte aligned memory and not 8.** `0x00203164`,
  `0x002182A4`, `0x0021A674` are all 4 mod 8, and `AllocMem(1)` came back
  `0x00218028`, which is 0 mod 8. `AllocVec` keeps the size in the longword in
  front of what it hands out, so it is `AllocMem`'s 8 minus 4 -- every time,
  not by luck. `docs/ALLOCATIONS.md` and `addralloc.c:90` both say "aligned
  enough for anything" and are right for this tree, where nothing wants more
  than 4; a future `double` or a 64-bit field in an `ami_alloc`ed struct would
  not be, and on m68k would not care either, since `__alignof__(double)` is
  also 2.

---

# Part 2 -- stack

An AmigaOS Process gets the stack its creator asked for and no more. There is no
guard page and no MMU: overflow writes over whatever is below and the machine
carries on until something else falls over, seconds later and somewhere else.
`bsd_WaitSelect`'s frame was cut from 864 bytes to 116 for this reason.

## How the numbers were got

`-fstack-usage` over a `-DAMINETXDUO_CPU=68000 -DCMAKE_BUILD_TYPE=Release`
build, 1,011 `.su` files, joined to a call graph disassembled out of the object
files, maximised over every path. So each figure is **the deepest chain of our
own and NetX Duo's frames, in bytes, for that entry point**, not a guess.

What the figures **exclude**, and none of it is small:

- **Exec and DOS calls.** `jsr -N(a6)` is opaque to the disassembler, and the
  SANA-II driver path makes `DoIO`/`SendIO`/`GetMsg` calls at the bottom of the
  deepest chains below.
- **Indirect calls** other than the NetX Duo link-driver entry, which is added
  by hand at the 26 vendored functions that call through
  `nx_interface_link_driver_entry`. Application hooks (`SBTC_ERROR_HOOK`, the
  net-monitor hook) are unbounded by construction and are the caller's problem.
- **ThreadX context-switch frames**, which sit on the thread's own stack.
- **Register save area**, where GCC's own accounting of it is incomplete.

Two artifacts run the other way and make the numbers slightly high: string
literals embedded in `.text` disassemble as `bsr` and can invent an edge. Four
such edges appeared as cycles; all four were opened by hand and none is a real
call. **There is no recursion anywhere in this tree** -- that is the SCC result,
with the four candidates checked individually.

## Every stack this code creates

| What | Where it is set | Bytes | Deepest measured | Verdict |
|---|---|---|---|---|
| bsdsocket bring-up Process | `library.c:321` `BSD_STARTUP_STACK` | 65,536 | 1,916 | clean -- 3% |
| TCP handler Process | `tcp_handler.c:96` `TCP_CTRL_STACK` | 8,192 | 116 | clean |
| TCP session Process | `tcp_handler.c:97` `TCP_SESSION_STACK` | 16,384 | 2,112 | clean -- 13%; `tcp_session_open` alone is a 920-byte frame |
| address-allocation worker | `addralloc.c:487` `BSD_AAM_STACK` | 8,192 | 608 | clean |
| ARexx port Process | `netstack_rexx.c:81` `RX_STACK` | 8,192 | 2,852 | clean -- 35%, the tightest Process. `ami_rx_service` is a **2,140-byte frame**, the largest in the shipping tree |
| Dropbear console reader | `amiga_dropbear.c:1090` | 8,192 | not measured -- `clients/` out of scope | |
| ThreadX kernel Task | `tx_initialize_low_level.c:1189` | 8,192 | 184 | clean |
| ThreadX tick Task | `tx_initialize_low_level.c:107` | 4,096 | 200 | clean |
| ThreadX timer thread | `tx_port.h:104` `TX_TIMER_THREAD_STACK_SIZE` | 4,096 | 56 | clean |
| NetX Duo IP thread | `netstack_internal.h:35` `AMI_IP_STACK_SIZE` | 4,096 | 972 | clean -- 24%, and the chain ends in `ami_sana2_driver_entry` |
| mDNS thread | `netstack_mdns.c:49` `AMI_MDNS_STACK_SIZE` | 4,096 | 1,516, up to ~1,820 through `nx_udp_socket_send` | clean but the tightest thread -- 44%. `_nx_mdns_thread_entry` is a 904-byte vendored frame we cannot shrink |
| AutoIP thread | `netstack_internal.h:37` `AMI_AUTOIP_STACK_SIZE` | 2,048 → **4,096** | 704 | **raised, see below** |
| SANA-II reader threads (2, or 3 with IPv6) | `sana2_internal.h:83` `AMI_SANA2_RX_STACK_SIZE` | 4,096 each | 280 | clean -- 7% |
| `fetch` | `fetch.c:1126` `FETCH_STACK_SIZE`, via `StackSwap` | 65,536 | 1,728 plus TLS | clean |
| ported clients | `clients/compat/amiga_argv.c:53`, via `StackSwap` | 262,144 | not measured | |
| Dropbear's command exec | `amiga_dropbear.c:1533` | 262,144 | not measured | |
| client smoke driver | `clients/dropbear/clientrun.c:140` | 524,288 | not measured | |
| **the caller's own stack** | whatever it has | -- | 1,208 | see below |

There is no `$STACK:` cookie anywhere and this toolchain's crt0 exports no
`__stack` hook, so every one of these is an explicit `NP_StackSize`,
`tx_thread_create` size, or `StackSwap`. That is stated in
`clients/dropbear/clientrun.c:11` and confirmed by grep.

### `AMI_AUTOIP_STACK_SIZE` 2048 → 4096 (RAISED)

The only sub-4K stack in the tree, and the reason it looked safe is that
NetX Duo's own samples use 2,048 for AutoIP. Those samples do not have this
driver. The measured chain is

```
_nx_auto_ip_thread_entry(88) > _nx_ip_interface_status_check(72)
  > ami_sana2_driver_entry(140) > ami_sana2_tx_send(60)
  > ami_bpf_tap_tx(120) > ami_bpf_capture(84) > ami_bpf_filter_view(104)  = 704
```

-- so this thread reaches a SANA-II device and makes Exec device calls, which
run on the current stack and are in none of that 704. 1,344 bytes of headroom
for an Exec call, on a machine with no fault and no guard page, is not a margin
worth defending. Every other thread that can reach the driver already has 4,096.

### What runs on the caller's stack, which is the real exposure

`port/threadx-amiga/src/tx_amiga_adopt.c:251` takes the calling Task's
`tc_SPUpper - tc_SPLower` and hands **that region** to `_tx_thread_create()` as
the ThreadX thread stack. So a library call does not switch stacks: NetX Duo,
and with `tls.library` the whole of nx_secure and the bignum code, run on
whatever the application had.

Measured over all 112 `bsd_*` symbols named in `bsdsocket_vectors.c` -- the
whole vector table plus its `bsd_enosys` stubs -- the deepest is
**`bsd_sendmsg` at 1,208
bytes**, and the chain is the same one as above -- through
`_nx_ipv6_packet_send` into `ami_sana2_driver_entry` and the BPF transmit tap.
`bsd_sendto` is 1,192, `bsd_send` 1,156, `bsd_accept` 1,100, `bsd_socket` 1,044.

A Kickstart 3.1 Shell gives a command 4,096 bytes and about 2,736 are left by
the time it reaches the library (measured, `fetch.c:1104`). 1,208 of 2,736 is
44%, before Exec. That works -- it is what every tool in `src/tools/` does today
-- but it is why `fetch` brings its own 64 KB and why `clients/compat` swaps to
256 KB before argv is even built.

`bsd_lib_open` measures 2,016, which is 74% of that 2,736 and would be alarming
if it were the normal path. It is not: the whole of it is `netstack_startup`,
and `bsd_netstack_bringup()` runs that on a Process of its own with 64 KB
(`library.c:333`). The 2,016 is only reachable through the two documented
fallbacks -- `AllocSignal` failed, or `CreateNewProc` failed -- which
`library.c:377` already describes as "an opener that came with enough stack still
works; one that did not is no worse off".

### The commands, added up

`main`'s own frame is exact from `.su`; the rest of each chain is the graph.

| | `main` frame | own chain | + deepest LVO | of 2,736 |
|---|---|---|---|---|
| `Online` / `Offline` | 476 | 1,152 | 2,360 | 86% |
| `NetTrace` | 828 | 1,060 | 2,268 | 83% |
| `tftp` | 780 | 980 | 2,188 | 80% |
| `ShowNetStatus` | 520 | 844 | 2,052 | 75% |
| `nslookup` | 360 | 744 | 1,952 | 71% |
| `traceroute` | 524 | 724 | 1,932 | 71% |
| `AddNetInterface` | 404 | 720 | 1,928 | 70% |
| `NetSetup` | 552 | 700 | 1,908 | 70% |
| `ping` | 472 | 680 | 1,888 | 69% |
| everything else | ≤ 420 | ≤ 636 | ≤ 1,844 | ≤ 67% |
| `fetch` | 48 | 72 | -- | swaps to 64 KB first |

Nothing overflows and nothing here is changed. The margin at the top of that
table is 300-500 bytes for the Exec and DOS calls the chain makes, which is
thin, and it is the reason the two big `main` frames -- `nettrace.c:905` at 828
and `tftp.c:575` at 780 -- are worth knowing about before either grows.

---

## Known and not done

- **`clients/` was not audited**, on either hazard. Dropbear, curl and the argv
  shim have their own casts and their own stacks; `amiga_argv.c`'s 256 KB
  `StackSwap` is why they get away with it, and the reason it exists is the same
  one `fetch.c` gives. `src/tlslib/` has four `(ULONG *)` casts
  (`tls_conn.c:288`, `:1018`, `:1058`, `:1065`) over caller tag data and
  `fd_set`s, all of which fall under the "clean by contract" rule above, but the
  library's own stack behaviour was not measured.
- **`src/crypto68k/` and `src/tls/` have no wide-cast sites at all** -- checked,
  not skipped. The hand-written 68020 assembly is not reachable on a 68000
  build (`AMINETXDUO_CRYPTO68K_ASM` is off there) and the portable C is
  index-based.
- **`mtod()`** (`include/aminetxduo/mbuf.h:182`) is BSD's "cast `m_data` to
  whatever you like" macro and has **zero call sites in this tree**. `m_data`
  moves by arbitrary amounts under `mbuf_adj()`, so the first internal user of
  it will be the first real instance of this bug class. Recorded because the
  `mbuf_*` LVOs are all `bsd_enosys` stubs today
  (`bsdsocket_vectors.c:132-142`) and nothing is reachable yet.
- **The stack figures do not include Exec, DOS or ThreadX switch frames.** They
  are a floor on our own code, not a total. Making them a total needs a
  measurement from a running machine -- filling a stack with a pattern at
  creation and reading the high-water mark back -- which is a different change.
- **`clients/`, `tests/` and `tools/smoke/` stacks were not measured**, only
  enumerated.
- **No Address Error was provoked.** `alignprobe` demonstrates the rules the
  audit rests on, and the `CMSG_BUFFER` defect before and after; it does not
  demonstrate a crash, because the fixed sites are fixed and the unfixed ones
  are unreachable from conforming code.
- **The library itself was not run on a 68000 in this pass**, only the probe.
  Booting `bsdsocket.library` on an A600 needs a SANA-II driver staged and the
  network harness, which is `tools/ci.sh emulator` and a different exercise. The
  cross build for `-DAMINETXDUO_CPU=68000` is in the default CI set and is
  green.
