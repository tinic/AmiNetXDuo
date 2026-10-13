# TCP TX changes verified against Emu68 emission

The four changes build on the RX branch at `e628d0d` (NetX Duo `65862f4`).
Comparison uses the actual linked bsdsocket.library, with `-O2 -m68020-60
-mregparm=3 -flto`, symbols retained, and the existing Linux Emu68 emitter
on the Pi. ARM is emitted, not executed. These are code-generation results;
throughput has not been measured for this candidate.

## Changes and generated ARM

1. **Check that the packet does not fit before evaluating sender SWS rules.**
   The helper is pure and its answer matters only when the pushed packet is
   larger than the available window. The new length comparison and branch
   at guest `0x1004c83a` / `0x1004c83c` emit 7 ARM instructions and jump over
   the SWS predicate when the packet fits. The previously unconditional SWS
   checks are absent from that path; SWS behavior for a packet that does not
   fit is retained.

2. **Keep IP protection through the offloaded TCP checksum path.**
   The offload branch at `0x1004ca06` jumps directly to packet capability
   marking and sequence advancement. It skips the mutex put/get calls,
   checksum calculation, sequence comparison and connection-state recheck.
   Software checksums still release protection, reacquire it, and retry or
   fail when the socket changed. The sequence is now captured under the lock.
   This also applies when checksum calculation is disabled; IPsec encryption
   still forces the protected software-checksum recovery path.

3. **Allocate and send directly on immediate BSD send success.**
   The TCP worker calls `_nx_packet_allocate` at `0x10009ec8` and
   `_nx_tcp_socket_send_internal` at `0x1000a00e` directly. Callback argument
   records and `bsd_wait_sliced` calls are on resource-failure paths. The old
   path invoked the sliced wrapper and its actor twice per segment. Each
   wrapper alone saved/restored eleven registers (6 + 7 emitted ARM
   instructions), in addition to calls, dispatch and actor overhead.
   The new path retains the break check before allocation and before send,
   and recomputes the remaining call budget before falling back. Allocation
   failure inside TCP send remains retryable, alongside window/queue pressure.
   The existing actors still flush a held transmit run before blocking.

4. **Handle normal SANA-II packet sends before the general command switch.**
   The normal command test at `0x10034850` / `0x10034852` emits 7 ARM
   instructions. The previous bounded switch dispatch emitted 24, including
   its indirect jump and unit exit. The entry now saves three registers and
   has no local frame, versus eleven registers and 92 local bytes before.
   Control commands use a private cold function; interface lookup, detachment,
   packet ownership, null-packet errors and status handling still run first.

Instruction counts describe the cited emitted sites, not whole-packet cycle
costs. The guest library text grows from 398180 to 398428 bytes (+248); the
changes reduce work on the successful transmit path. The private TCP worker
has no HUNK symbol: its evidence label is assigned from the disassembly,
not from the unrelated nearest public symbol.

## Verification

- Normal production LTO link and exact PC-relative branch check pass:
  157 checked branches, no mismatches or unscanned branches.
- Existing native `tcp_sws`, `tcp_cork_send`, `tcp_persist`, `tcp_rxpass`,
  `tcp_rxflood`, `sana2_driver`, and `sana2_tx` targets pass.
- Existing 32-bit `transfer_scatter_gather` test passes 127 checks using its
  normal host32 flags. Added checks cover direct success, finite timeout
  sharing across a tick wrap, pending/inter-segment breaks, partial-send
  credit on interruption and retry, packet-shortage retry, and flush before
  allocation/send waits. The final-refusal partial-send case uses a
  nonblocking socket so the scripted refusal ends the operation.
- Added cork-send checks verify one mutex get/put for an offloaded aligned
  segment, two for a software-checksummed segment, nested lock balance,
  checksum retry after a sequence change, and disconnect during calculation.
- Final Emu68 dumps for the TCP send function, SANA-II entry and private BSD
  TCP worker all complete without unsupported instructions.

Small annotated ARM samples, build/test logs and the linked-image manifest
are retained in `/Users/turo/ai/evidence/emu68-tx-jit/`. No new harness or
runtime environment is added to either repository.

## Follow-up: append and checksum setup

Based on the TX branch at `df8511eb`, using the same linked build profile and
existing Pi Emu68 emission tools. No harness changes are required. NetX Duo
remains pinned at `fb53e83e`; these changes belong in AmiNetXDuo's integration.

| Selected ARM work | Before | After |
| --- | ---: | ---: |
| Single-buffer append around the copy | 102 | 54 |
| Common GENET IPv4/TCP checksum checks | 136 | 85 |
| TCP pseudo-header reads, sum and folding | 46–59 | 40 |

These are counts from selected emitted slices, with context-dependent CCR
expansion. They exclude copy bodies and JIT unit exits. They are not a cycle
profile or a throughput prediction. The previous A1200 A/B results apply to
`df8511eb`, not this follow-up.

### Single-buffer append

`bsd_packet_append_iov()` copies directly when there is no packet chain or tail
and the current buffer has enough capacity for the entire contiguous iovec run.
It updates the append pointer and packet length, then advances the cursor as
before. The copy uses the same `memcpy` hook as NetX's native append. The native
routine handles insufficient capacity, chains, event tracing and packet debug
builds, preserving allocation errors and partial-send credit.

The candidate's selected slices are guest `0x10009b56..0x10009b5c`,
`0x10009bc0..0x10009bc6`, `0x10009bc6..0x10009bd4`,
`0x10009bd4..0x10009be2`, and `0x10009be2..0x10009bf2`.
They total 54 ARM instructions, including guards and copy-call setup. The
previous selected native append slices totalled 102. The surrounding iovec
iteration remains; the native append call and its frame are skipped.

### Checksum setup

The driver recognizes a normal 20-byte IPv4 TCP header and validates its total
length, fragment flags/offset, negotiated offload capability and TCP header
length before publishing the fixed checksum offset. IPv4 options, UDP and
other shapes retain the general parser. Parser arithmetic uses full-width
values, reducing partial-register work.

The selected common driver slices at `0x1000529e..0x100052c2`,
`0x10005392..0x1000539a`, and `0x100053a0..0x100053da` total 85 ARM instructions.
The old 136-instruction selection excluded the final offset bound; the new
selection includes constant output preparation, and its validated fixed offsets
make that bound unnecessary.

The SANA-II pseudo-header helper uses explicit network-word loads on 68020+
(which permit odd addresses), with byte reads on 68000 builds and other hosts.
GCC kept expanding the aligned C form into byte sequences, so the target load
is a small inline-assembly operation with a packed alias-safe memory operand.
Four address words plus protocol and IPv4 TCP length fit in 19 bits; two fixed
carry folds replace the branch-dependent folding sequence. The complete
preparation at `0x1002f592..0x1002f5dc` emits 40 ARM instructions, compared with
46, 57 or 59 in the old selection depending on carries.

### Verification

Eight existing native CTest targets pass: SANA-II copy, real NetX checksum copy,
TX, driver entry, netdev verification, TCP cork send, packet extraction and trace
extraction. Existing 32-bit fixtures pass 133 transfer, 509 cork and 530 fast-cork
checks. Added cases cover exact buffer fit across iovecs, allocation refusal after
a direct append, chains, IPv4 options, DF versus fragments, short TCP headers,
odd-address pseudo headers and a carry requiring a second fold.

Both linked-binary PC-relative checks pass with zero mismatches or unscanned
branches. All requested final Emu68 dumps succeed. Compact annotated evidence
and image hashes are retained in `/Users/turo/ai/evidence/emu68-tx-followup`.
No hardware throughput result is claimed for these changes.
