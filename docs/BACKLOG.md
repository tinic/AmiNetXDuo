# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| `ne2000_pcmcia` will not come up, and it is in 0.21.2 | `iface_rc=20`, `tx_bytes=0` in the tag sweep; it passed on `main` the same morning. Either the X-Surf multicast fallback arms on `cnet.device` or the rebuilt emulator did it — the two are separable and being bisected | `src/sana2/sana2_device.c:409-517` or `qemuvga/ne2000.cpp:1273` |
| `run-ifquery.sh` cannot pass bridged | Its DHCP assertions are written against SLIRP's fixed answers, so `the lease does not carry the server's own numbers` and `no router came back` fail against a real DHCP server. SLIRP is not an option, so the harness has no passing configuration | `tests/tools/run-ifquery.sh` |
| The `-w` peer capture cannot run | The fitz harnesses run `tcpdump` on the peer under `sudo`, which needs a password on playhouse4, so the documented flag does nothing there. A capability-bearing private copy is the workaround | `tests/perf/run-fitzbench.sh` `-w` |
| `run-family.sh` reads IPv6 before SLAAC finishes | It samples immediately after `AddNetInterface`, so a link-local still `(tentative)` reports all 13 `-6` arms as `blocked` and it exits 3. `run-multiaddr.sh` waits 20 s on the same wire and never sees it | `tests/tools/run-family.sh` |
| Nothing gates off-LAN IPv6 per card | The `tests/ipv6` runners are WinUAE or SLIRP only, so the defect that left three shipping cards without off-LAN IPv6 had no harness that could have caught it | `tests/ipv6/` |
