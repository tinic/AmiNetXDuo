# Backlog

What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.

**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.

| Item | Why it is open | Cite |
|---|---|---|
| Encrypt-then-MAC, RFC 7366 | Answers the CBC padding-timing risk at no per-record cost; constant-time padding costs ~10 ms per record at 7 MHz | RFC 7366 |
| Extended master secret, RFC 7627 | Without it resumption is open to the triple-handshake attack. Key-schedule change; invalidates every cached session | RFC 7627 |
| EKU, nameConstraints and critical-extension rejection | Must land together: honouring the critical bit without enforcing nameConstraints is worse than neither. Untestable without hardware | `nx_secure_x509_extension_find.c:191` |
| `src/bsdsocket` has one host test | `test_inet` reaches 7 of 29 files. 80 `_Static_assert`s hold the ABI; what is unheld is behaviour needing the real ABI, so it is guest-suite work | `src/bsdsocket/` |
| `src/tlslib`: 4,628 lines behind three | No test of the handshake, of the record layer beyond the fuzzers, or of ticket and session-ID resumption | `src/tlslib/` |
| `src/sana2` has one test | `sana2_copy.c` alone of 3,704 lines. The rest runs at interrupt time, where a mistake takes the machine down | `src/sana2/` |
