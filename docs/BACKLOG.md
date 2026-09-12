# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
**ONE LINE PER ROW.** What is wrong and where. Not how it was found.
| Item | Why it is open | Cite |
|---|---|---|
| `stage_console` runs (`run-console.sh -c playhouse2 -C ham6 -m A1200` gave `RESULT=PASS`) and no workflow invokes it | still allowlisted in `tools/check-stage-coverage.sh`, so it proves nothing on a push | `tools/ci.sh`, `.github/workflows/emulator.yml` |
| A workflow arm whose `-r` variable is unset SKIPS and reports success | `AMINETXDUO_RATE_IFACE` and `AMINETXDUO_RATE_PEER` are undefined in CI, so the throughput gate has never run there and the step shows green; the wiring gates check that a stage is named, not that it ran | `tools/ci-arm.sh:78-90` |
| The guru after `NetShutdown` is unreproduced | `run-hwcard.sh -a shutdown` tells `machine_stopped` from `network_did_not_return`; it needs a run on the real A1200, which answers when it is up | `tests/tools/run-hwcard.sh` |
| The CNet16 probe-order fix has a host test and has never met the card | Amiberry decodes both windows 1:1 and cannot hold a card that refuses a byte read, so only `run-hwcard.sh -a probeorder` on the real A1200 exercises the retry | `src/netdev/ne2000.c:401`, `src/netdev/test/test_netdev_ne2000.c` |
| `pc_settle()`'s own loop is still underived from a measurement | the spins-per-raster-line figure is measured (25 and 27 against a floor of 252); `pc_settle` reads PCMCIA attribute memory, not `$DFF004`, so its constant differs and only the direction carries over | `tests/tools/run-gayleratio.sh:221` |
| Roadshow reads 31 KB/s more than we do on its own `x-surf-100.device` | 949 against 918 on a user's A3000; our `anxnet.device` reads 938 there. The lab cannot price it: Amiberry answers a longword with two `ne2000_wget` calls, which inverts the ordering | `tools/emu-board.sh`, `src/netdev/netdev_cards.c` |
| The 3c589 RX FIFO-hold fix has no wire number | `run-hwcard.sh -a overruns` reads `netstat -s` either side of a 4 MB receive; the effect is the difference between two runs with the two libraries at the same load, on the real A1200 | `tests/tools/run-hwcard.sh`, `src/netdev/test/test_netdev_el3.c:35` |
