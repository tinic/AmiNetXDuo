# Which condition posts which event

`anxnet.device` only. One row per condition that reaches `S2_ONEVENT`.

`S2EVENT_ERROR` is a qualifier, not a code: the spec's worked example
(`sana2device.spec:1067-1071`) says a buffer failure during receive returns
`ERROR`, `RX` and `BUFF` together, so `ERROR` never travels alone here.
`cnet.device` drops it on its two BUFF sites and slip.device does the same; the
spec wins. A waiter matches if ANY bit overlaps, so a request for `ERROR` alone
is woken by all of these.

`ios2_WireError` carries the whole posted mask, not the intersection with what
was asked for, so a waiter learns where the error was and not merely that one
happened.

| Posted | Condition | Site |
|---|---|---|
| `ERROR\|RX` | frame shorter than a header | `netdev_device.c` `netdev_rx` |
| `ERROR\|RX` | frame of a type no opener wanted | `netdev_rx` |
| `ERROR\|RX` | chip receive errors or overruns moved | `netdev_server` |
| `ERROR\|RX\|BUFF` | an opener's `CopyToBuff` returned FALSE | `netdev_hand_over` |
| `ERROR\|TX` | write longer than the MTU | `netdev_tx_build` |
| `ERROR\|TX` | the chip core refused the frame | `netdev_tx_pump`, `netdev_tx_direct` |
| `ERROR\|TX` | chip transmit errors moved | `netdev_server` |
| `ERROR\|TX\|BUFF` | an opener's `CopyFromBuff` returned FALSE | `netdev_hand_over` |
| `ERROR\|HARDWARE` | the chip core refused to initialise | `netdev_online` |
| `ERROR\|TX\|HARDWARE` | the watchdog reset a wedged transmitter | `netdev_tick` |
| `ERROR\|OFFLINE\|HARDWARE` | the PCMCIA card was pulled | `netdev_pcmcia.c` |
| `ONLINE` / `OFFLINE` | the unit went up or down | `netdev_online`, `netdev_offline` |

Not posted, deliberately: `S2EVENT_TX` on a successful transmit — cnet's two
`TX` sites are both refusals — and `S2EVENT_SOFTWARE`, which is refused at
`S2_ONEVENT`; see `CONFORMANCE.md`.

Chip counters are diffed once in the interrupt server rather than raised by each
core, which is why no card file posts an event.

## Cost when nobody is listening

`nu_EventMask` is the union of every queued request's mask, so a post with no
waiter is one word read and a branch. It is a `UWORD`: it is read without the
lock, and a 68000 reads a long in two bus cycles, so a `ULONG` could tear. All
eight `S2EVENT_*` bits fit a word. It is recomputed at queue, at completion, and
by `netdev_event_rescan()` from `CMD_FLUSH`, `AbortIO` and close.

## Interrupt context

Every poster but `S2_ONLINE` runs at interrupt: the INT2 card server, the INT3
watchdog, and card.resource's removal callback. The requests belong to other
tasks, so `netdev_event()` takes `Disable()` — `Forbid()` is not exclusion
against the INT2 server walking the same list — removes each request BEFORE
replying, and does nothing else. No allocation, no `Wait`, no semaphore.
`ReplyMsg` is a `PutMsg` plus a `Signal` and is interrupt-callable. `Disable()`
nests, so a caller already holding it is not a special case.

The host test's stub `ReplyMsg` fails if a request is replied while still linked
into a list. That is the bug this file exists to prevent, and an emulator will
not catch it.

## The filter

`S2_PacketFilter` takes a `utility.library` Hook, not a function pointer:
`a0` = hook, `a2` = the `IOSana2Req`, `a1` = the packet data, result in `d0`.
It runs after the addresses, type, length and flags are filled in and before
`CopyToBuff` (`standard.txt:301-303`). The data pointer excludes the 14-byte
header unless RAW was asked for.

TRUE hands the packet over. FALSE returns the `CMD_READ` to its queue with
`AddHead`, un-replied, and the frame goes on to the next opener and then to
`S2_READORPHAN` — a rejection is neither a delivery nor a drop, so it does not
count in `PacketsDropped`. slip.device does the same.

The spec defines the filter for receive only; there is no transmit-side hook.
Applying it to `S2_READORPHAN` as well as `CMD_READ` is a judgement, not a
citation.

SPDX-License-Identifier: MIT
