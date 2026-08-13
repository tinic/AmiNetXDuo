# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| `run-ifquery.sh` cannot pass bridged | Its DHCP assertions are written against SLIRP's fixed answers, so `the lease does not carry the server's own numbers` and `no router came back` fail against a real DHCP server. SLIRP is not an option, so the harness has no passing configuration | `tests/tools/run-ifquery.sh` |
| The lab emulator has no IPv6 repair built in yet | `4c4cff0a` merged the IPv6 partial-checksum fix, but `~/amiberry/build/amiberry` is still the 03:38 binary because measurements were running against it. Every off-LAN IPv6 figure until it is rebuilt carries the old artifact | `~/amiberry` on playhouse3 |
| `fetch` tries one address and gives up | It takes the first answer from the resolver and connects to that alone, so a name whose first address is unreachable costs a 191 s timeout and then fails, with the other family's working address never tried | `src/tools/fetch.c:554-583`, `tool_sock_resolve_af` |
| A clean-link transfer aborts mid-stream, rarely | `Read() gave 0 of 32768`, no connection lost. 2 arms on `c3e4e806`-based builds, base and cap alike; 0 in ~30 arms before it. Confounded: another agent's unscoped `pkill -f fitz-serve` on the shared peer produces the same signature. Needs a quiet rig to separate | `run-fitzbench.sh` `RESULT read FAILED` |
| Nothing gates off-LAN IPv6 per card | The `tests/ipv6` runners are WinUAE or SLIRP only, so the defect that left three shipping cards without off-LAN IPv6 had no harness that could have caught it | `tests/ipv6/` |
