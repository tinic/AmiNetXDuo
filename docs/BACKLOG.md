# Backlog

What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.

**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.

| Item | Why it is open | Cite |
|---|---|---|
| `bsd_send_consumed()` on a multi-segment partial send | The one place in the library where a wrong answer duplicates stream bytes. `tcpdrill z02/z03` credit a short send but never drive the trim loop | `transfer.c:375` |
| `MSG_WAITALL`, `SO_LINGER{1,n>0}`, `bsd_wait_errno()` under pool exhaustion | Zero coverage. A wire script cannot block, so tcpdrill cannot reach them; they need a guest program | `transfer.c:969`, `socket.c:854`, `errno.c:224` |
| The URG transmit checksum patch, RFC 1624 eq. 3 | The most checksum-fragile code in the tree; `tcpdrill u01` covers receive only | `oob.c:179` |
| `bsd_accept()` EMFILE arm stored into a freed socket | Fixed by inspection, not by a test — it needs fd-table exhaustion coinciding with a refused relisten, and its symptom is corruption, not a wire event | `socket.c` |
| Nothing in `emulator.yml` has ever run | `vars.AMINETXDUO_KICKSTART_RUNNER` is unset and no self-hosted runner is registered, so the kickstart job is skipped nightly — every Emulator run completes in 6-14 s doing only the tier report. All 22 wired harnesses, not just the recent ones. `tools/ci.sh <stage>` on playhouse3 is what runs them today | `.github/workflows/emulator.yml` |
| fs-uae is gone but the tree still routes through it | `tools/fsuae-run.sh` is invoked by nothing yet ~10 comments send manual work through it and it still shells out to the `fs-uae` binary; `tests/conformance/run-fsuae.sh:120` writes an fs-uae config while driving Amiberry; `netpeer.py:5` documents an fs-uae SLIRP workflow | `tools/fsuae-run.sh` |
| `alloc-census-known.txt:44` can never match | The gate key is `library.c:636`; `AMI_CENSUS_ADD` stamps `library.c:761`, so the child-base block is unaccounted in the census | `tools/alloc-census-known.txt:44` |
