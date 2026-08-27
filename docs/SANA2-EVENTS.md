# Which condition posts which event

`anxnet.device` only. One row per condition that reaches `S2_ONEVENT`.

`S2EVENT_ERROR` is a qualifier, not a code: the spec's worked example
(`sana2device.spec:1067-1071`) returns `ERROR`, `RX` and `BUFF` together, so
`ERROR` never travels alone here — `cnet.device` and `slip.device` drop it on
their BUFF sites, and the spec wins. A waiter matches if ANY bit overlaps, so a
request for `ERROR` alone is woken by all of these. `ios2_WireError` carries the
whole posted mask, not the intersection with what was asked for, so a waiter
learns where the error was and not merely that one happened.

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

## Interrupt context, the mask, and the filter

`src/netdev/netdev_event.c` is the implementation and its header is the
statement: the `nu_EventMask` fast gate and why it is a `UWORD`, the `Disable()`
that every poster but `S2_ONLINE` needs because the INT2 card server walks the
same list, and the `S2_PacketFilter` hook's register shape.

| Rule | Effect |
|---|---|
| A request is removed from its list BEFORE `ReplyMsg` | the host test's stub `ReplyMsg` fails if a request is replied while still linked. That is the bug this file exists to prevent, and an emulator will not catch it |
| The filter runs after the addresses, type, length and flags are filled in and before `CopyToBuff` (`standard.txt:301-303`) | the data pointer excludes the 14-byte header unless RAW was asked for |
| FALSE returns the `CMD_READ` to its queue with `AddHead`, un-replied | the frame goes on to the next opener and then to `S2_READORPHAN`. A rejection is neither a delivery nor a drop, so it does not count in `PacketsDropped`; slip.device does the same |
| The spec defines the filter for receive only | there is no transmit-side hook, and applying it to `S2_READORPHAN` as well as `CMD_READ` is a judgement, not a citation |

SPDX-License-Identifier: MIT
