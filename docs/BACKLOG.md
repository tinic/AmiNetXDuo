# Backlog

What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.

**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.

| Item | Why it is open | Cite |
|---|---|---|
| TLS 1.3 handshake dies in certificate verification | Socket goes CLOSED locally from Certificate onward, with no server segment in between. `NOVERIFY` completes | `nx_secure_tls_1_3_client_handshake.c`, `tests/tls/run-tls13.sh` |
| TLS 1.3 client ships but is red | Fails at `nx_secure_tls_process_record.c:558` on a bad content type, so the key schedule derives wrong keys. Server-side 1.3 is impossible: nx_crypto has PSS verify, no PSS sign | `src/tls/CMakeLists.txt:99` |
| Encrypt-then-MAC, RFC 7366 | Answers the CBC padding-timing risk at no per-record cost; constant-time padding costs ~10 ms per record at 7 MHz | RFC 7366 |
| Extended master secret, RFC 7627 | Without it resumption is open to the triple-handshake attack. Key-schedule change; invalidates every cached session | RFC 7627 |
| RFC 2308 §5 negative cache | A name that does not exist is looked up again on every call. Needs a synthetic entry to hold the SOA MINIMUM | `nxd_dns.c:3587` |
| Bailiwick check with CNAME chain following | `NX_DNS_ENABLE_EXTENDED_RR_TYPES` is undefined, so an A record after a CNAME passes only because no owner check exists. The check alone breaks every CNAME-hosted name | `nxd_dns.c` |
| EKU, nameConstraints and critical-extension rejection | Must land together: honouring the critical bit without enforcing nameConstraints is worse than neither. Untestable without hardware | `nx_secure_x509_extension_find.c:191` |
| `src/bsdsocket` has one host test | `test_inet` reaches 7 of 29 files. 80 `_Static_assert`s hold the ABI; what is unheld is behaviour needing the real ABI, so it is guest-suite work | `src/bsdsocket/` |
| `src/tools`: 32,412 lines behind five host tests | All five are httpd's. The other twenty-five commands have none, and the 2026-08-04 diagnostics rewrite touched every one | `src/tools/` |
| `src/tlslib`: 4,628 lines behind three | No test of the handshake, of the record layer beyond the fuzzers, or of ticket and session-ID resumption | `src/tlslib/` |
| `src/sana2` has one test | `sana2_copy.c` alone of 3,704 lines. The rest runs at interrupt time, where a mistake takes the machine down | `src/sana2/` |
| A command is mostly C runtime | `ping` is 16,196 bytes, about 2,050 of it ours. libnix's crt0 pulls in stdio and the C++ AVL allocator | `src/tools/CMakeLists.txt` |
| `run-tcphandler.sh` hardcodes its peer | 10.0.2.2 written out seven times, which is what stops it moving to a bridged backend | `tests/tools/run-tcphandler.sh` |
| SMB has no coverage in CI | The only harness that mounts SMB is on `smb-mount-e2e` and `smb-mount-smbfs`, both unmerged. Two user reports were unreproducible for a day because of that | `install/test/run-smbmount.sh` |
| `run-netshutdown.sh` and `run-lossgate.sh` are invoked by nothing | The loss gate is the only rig that can price an ack or retransmit change, and it still needs a `-B` baseline recorded | `tests/perf/run-lossgate.sh` |
