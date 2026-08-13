# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| `ShowNetStatus` cannot show the IPv6 destination table | It prints the router table and the ND cache, so the table that silently dropped every third destination was invisible to the one command that should have shown it | `src/bsdsocket/netstatus.c:610,657` |
| `whois -6` calls a timeout a refusal | Reports `connection refused` after 191 s, which is the connect ladder running out, not an RST | `src/tools/whois.c` |
| A card that carries bytes but fails an assertion is scored a skip | `run-cardsweep.sh` exits 3 for `fail_assert`, and `tools/ci.sh:840` maps 3 to skip, so a real assertion failure is quiet unless some other card fails outright | `tests/tools/run-cardsweep.sh:438`, `tools/ci.sh:840` |
| Unpaced `iperf -u` is 8x slower than paced | 1.00 Mbit/s flat out against 8.00 Mbit/s at `-b 8000` with nothing lost. Unchanged by the pacing fix, so it is its own defect | `src/tools/iperfcore.c` |
| `ariadne` never gets a global IPv6 address | Online with a DHCP lease and IPv4 off-LAN working, link-local formed, no RA ever arrives. A capture across a whole boot caught zero ICMPv6 from that guest — no router solicitation, no DAD — while another guest's RS and the router's RA were seen in the same window | found by `run-cardsweep6.sh`; stack or emulator, not separated |
