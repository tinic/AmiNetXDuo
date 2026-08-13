# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| A skipped arm renders as a green PASS in the GitHub summary | `ci-arm.sh:117-123` maps exit 0 to `status=pass` whether the stage ran or skipped, which is why the cardsweep masking was total rather than amber. Changing it changes every arm's reporting | `tools/ci-arm.sh:117-123` |
| Four harnesses skip their only off-box assertion and exit 0 | Green with the substance untested | `run-dnscache.sh:190`, `run-routes.sh:270`, `run-iperf.sh:631`, `run-tcpdrill.sh:75` |
| `test-verdict.sh` reports 'skipped' over real failures | It greps for the string SKIPPED and returns 77 before checking the failure count. Still exits non-zero, so red — a wrong message, not a hole | `tools/test-verdict.sh:121-126` |
| `whois -6` calls a timeout a refusal | Reports `connection refused` after 191 s, which is the connect ladder running out, not an RST | `src/tools/whois.c` |
| Unpaced `iperf -u` is 8x slower than paced | 1.00 Mbit/s flat out against 8.00 Mbit/s at `-b 8000` with nothing lost. Unchanged by the pacing fix, so it is its own defect | `src/tools/iperfcore.c` |
