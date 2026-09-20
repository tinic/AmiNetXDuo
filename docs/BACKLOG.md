# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
**ONE LINE PER ROW.** What is wrong and where. Not how it was found.
| Item | Why it is open | Cite |
|---|---|---|
| The ZZ9000 console offload firmware is staged but has not been activated | The `0x8200` handler is built into ZZ9000OS and `BOOT.bin` is on the A3000's card with `BOOT.bak` retained; a cold power cycle must load it, then `/console` must prove the service and fallback on the A3000 before the row closes | `docs/plans/zz9000-console-offload.md`, `src/tools/httpzz.c`, `src/tools/httpfb.c` |
| The CNet16 probe-order fix has a host test and has never met the card | Amiberry decodes both windows 1:1 and cannot hold a card that refuses a byte read, so only `run-hwcard.sh -a probeorder` on the real A1200 exercises the retry | `src/netdev/ne2000.c:402`, `src/netdev/test/test_netdev_ne2000.c` |
| Roadshow reads 31 KB/s more than we do on its own `x-surf-100.device` | 949 against 918 on a user's A3000; our `anxnet.device` reads 938 there. The lab cannot price it: Amiberry answers a longword with two `ne2000_wget` calls, which inverts the ordering | `tools/emu-board.sh`, `src/netdev/netdev_cards.c` |
| The 3c589 RX FIFO-hold fix has no wire number | `run-hwcard.sh -a overruns` reads `netstat -s` either side of a 4 MB receive; the effect is the difference between two runs with the two libraries at the same load, on the real A1200 | `tests/tools/run-hwcard.sh`, `src/netdev/test/test_netdev_el3.c:35` |
| The fork changes have never been offered to eclipse-threadx | 247 non-merge commits and 16,897 lines of shipping code diverge from `upstream/dev`; the survey, the tranche order and the per-PR cost are measured, the scope decision is not taken | `docs/UPSTREAMING.md` |
| Audit 2026-09-20, source cohesion: three shipping monoliths remain (`httpd.c` 7468, `netstack.c` 4247, `netdev_device.c` 2714); the HTTP server still combines protocol dispatch, WebDAV filesystem traversal and three browser applications, while the stack and device files each combine lifecycle with policy | one concern per file without changing protocol, lifecycle or hardware behaviour | `src/tools/httpd.c`, `src/netstack/netstack.c`, `src/netdev/netdev_device.c` |
