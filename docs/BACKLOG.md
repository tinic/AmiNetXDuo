# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| `run-ifquery.sh` cannot pass bridged | Its DHCP assertions are written against SLIRP's fixed answers, so `the lease does not carry the server's own numbers` and `no router came back` fail against a real DHCP server. SLIRP is not an option, so the harness has no passing configuration | `tests/tools/run-ifquery.sh` |
| `run-family.sh` opens a2065.device whatever board it boots | `DEVICE=a2065.device` is hardcoded, so `-N xsurf` boots an X-Surf and the guest still opens the wrong driver and fails everything | `tests/tools/run-family.sh:438` |
| A card that carries bytes but fails an assertion is scored a skip | `run-cardsweep.sh` exits 3 for `fail_assert`, and `tools/ci.sh:840` maps 3 to skip, so a real assertion failure is quiet unless some other card fails outright | `tests/tools/run-cardsweep.sh:438`, `tools/ci.sh:840` |
| Unpaced `iperf -u` is 8x slower than paced | 1.00 Mbit/s flat out against 8.00 Mbit/s at `-b 8000` with nothing lost. Unchanged by the pacing fix, so it is its own defect | `src/tools/iperfcore.c` |
| `run-family.sh` reads IPv6 before SLAAC finishes | It samples immediately after `AddNetInterface`, so a link-local still `(tentative)` reports all 13 `-6` arms as `blocked` and it exits 3. `run-multiaddr.sh` waits 20 s on the same wire and never sees it | `tests/tools/run-family.sh` |
| `ariadne` never gets a global IPv6 address | Online with a DHCP lease and IPv4 off-LAN working, link-local formed, no RA ever arrives. A capture across a whole boot caught zero ICMPv6 from that guest — no router solicitation, no DAD — while another guest's RS and the router's RA were seen in the same window | found by `run-cardsweep6.sh`; stack or emulator, not separated |
