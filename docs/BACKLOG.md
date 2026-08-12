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
| Source comments that the tree contradicts | Six say "the 4 MB floor"; the measured floor is 1 MB. `netstack.h:138` and `netx_call.c:38` blame the per-call bracket for losing bulk transfer to Roadshow, measured flat. `sana2.h:1-13` describes the pre-correction SANA-II model and contradicts its own code. Nine RESEARCH cites resolve to the wrong row | `include/aminetxduo/netstack.h:138`, `src/bsdsocket/netx_call.c:38`, `include/aminetxduo/sana2.h:1` |

