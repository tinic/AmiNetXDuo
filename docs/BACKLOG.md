# Backlog

What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.

**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.

| Item | Why it is open | Cite |
|---|---|---|
| `bsd_accept()` EMFILE arm stored into a freed socket | Fixed by inspection, not by a test — it needs fd-table exhaustion coinciding with a refused relisten, and its symptom is corruption, not a wire event | `socket.c` |
| Nothing in `emulator.yml` has ever run | `vars.AMINETXDUO_KICKSTART_RUNNER` is unset and no self-hosted runner is registered, so the kickstart job is skipped nightly — every Emulator run completes in 6-14 s doing only the tier report. All 22 wired harnesses, not just the recent ones. `tools/ci.sh <stage>` on playhouse3 is what runs them today | `.github/workflows/emulator.yml` |
| fs-uae is gone but the tree still routes through it | `tools/fsuae-run.sh` is invoked by nothing yet ~10 comments send manual work through it and it still shells out to the `fs-uae` binary; `tests/conformance/run-fsuae.sh:120` writes an fs-uae config while driving Amiberry; `netpeer.py:5` documents an fs-uae SLIRP workflow | `tools/fsuae-run.sh` |
| `alloc-census-known.txt:44` can never match | The gate key is `library.c:636`; `AMI_CENSUS_ADD` stamps `library.c:761`, so the child-base block is unaccounted in the census | `tools/alloc-census-known.txt:44` |
