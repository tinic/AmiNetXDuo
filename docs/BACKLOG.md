# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| A skipped arm renders as a green PASS in the GitHub summary | `ci-arm.sh:117-123` maps exit 0 to `status=pass` whether the stage ran or skipped, which is why the cardsweep masking was total rather than amber. Changing it changes every arm's reporting | `tools/ci-arm.sh:117-123` |
| Four harnesses skip their only off-box assertion and exit 0 | Green with the substance untested | `run-dnscache.sh:190`, `run-routes.sh:270`, `run-iperf.sh:631`, `run-tcpdrill.sh:75` |
| `test-verdict.sh` reports 'skipped' over real failures | It greps for the string SKIPPED and returns 77 before checking the failure count. Still exits non-zero, so red — a wrong message, not a hole | `tools/test-verdict.sh:121-126` |
| The loss gate's captures are unreadable by its own parser | `peercap.sh` captures on `any`, producing LINUX_SLL2; `lossrate.py` rejects link type 276, so `-w`/`-l`/`-L` collect a capture nothing reads and gate on nothing. Set `AMINETXDUO_PEER_IFACE=ens18` or teach `packets()` the SLL2 header | `tests/perf/lossrate.py`, `tests/perf/peercap.sh` |
| Why is real X-Surf hardware at half speed when emulation says we lead | Shootout: we beat AmiTCP_NG by 24% and Roadshow by 8% on the emulated card, profile shows no extra byte-walk, so the 2x is not stack architecture. Open hypothesis: ACK-clock starvation on a slow real receiver. Needs bifat's data — above all the sign of his 0.21.0 vs 0.21.3 delta | `~/anxd-xsurfgap/build/` on playhouse3 |
| `whois -6` calls a timeout a refusal | Reports `connection refused` after 191 s, which is the connect ladder running out, not an RST | `src/tools/whois.c` |
| Unpaced `iperf -u` is 8x slower than paced | 1.00 Mbit/s flat out against 8.00 Mbit/s at `-b 8000` with nothing lost. Unchanged by the pacing fix, so it is its own defect | `src/tools/iperfcore.c` |
