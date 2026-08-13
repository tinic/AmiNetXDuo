# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| A down interface leaks its config job for ever | `BeginInterfaceConfig` is never replied on an interface staged down, so the `bsd_aam_jobs[]` entry stays claimed and every allocation after it answers `AAMR_Busy` — ten failures downstream of one. Never run bridged until now | `src/bsdsocket/` AAM job table |
| IPv6 to a fresh destination stops working late in a long run | `fetch -6`, `whois -6` and `sntp -6` fail near the end of a 53-command run on a2065 and ne2000_pcmcia, while earlier-used destinations keep working. A fresh boot with only those commands passes against the same addresses | stack, not separated |
| `whois -6` calls a timeout a refusal | Reports `connection refused` after 191 s, which is the connect ladder running out, not an RST | `src/tools/whois.c` |
| A card that carries bytes but fails an assertion is scored a skip | `run-cardsweep.sh` exits 3 for `fail_assert`, and `tools/ci.sh:840` maps 3 to skip, so a real assertion failure is quiet unless some other card fails outright | `tests/tools/run-cardsweep.sh:438`, `tools/ci.sh:840` |
| Unpaced `iperf -u` is 8x slower than paced | 1.00 Mbit/s flat out against 8.00 Mbit/s at `-b 8000` with nothing lost. Unchanged by the pacing fix, so it is its own defect | `src/tools/iperfcore.c` |
| `ariadne` never gets a global IPv6 address | Online with a DHCP lease and IPv4 off-LAN working, link-local formed, no RA ever arrives. A capture across a whole boot caught zero ICMPv6 from that guest — no router solicitation, no DAD — while another guest's RS and the router's RA were seen in the same window | found by `run-cardsweep6.sh`; stack or emulator, not separated |
