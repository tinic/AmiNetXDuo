# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
| Item | Why it is open | Cite |
|---|---|---|
| `<name>.local` never answers a query | The guest announces `A <addr>` at boot, then answers neither `dns-sd` nor a raw multicast query, so a machine is reachable only by address | `src/netstack/` mDNS responder |
| `demo.sh` misses the lease of an idle guest | It waits for an ARP *reply*, which needs someone to ARP the guest first; a booted, configured guest nobody talks to times out at 400s | `tools/demo.sh:411` |
