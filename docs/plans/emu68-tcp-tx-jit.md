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
