# Backlog

What is outstanding, what was decided against, and why. Findings come mostly from
the NDK 3.2 autodoc audit (four passes over all 122 documented entries) and from
the memory-floor and stress work.

The autodoc is at `NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc`. **`grep` silently
fails on it** — `file` misidentifies it as "GTA in-game text". Read it with python.

---

## Open — no decision taken

- **Two tasks adding an interface at once can pick the same slot.**
  `netstack_interface_add()` reads `ami_ns_free_interface_slot()`, then opens the
  SANA-II device — Exec I/O, and long — before anything records the slot as
  taken. `ami_ns_lock` guards only startup and shutdown, and the ThreadX bracket
  cannot serve: `netstack_interface_add()`/`_remove()` run most of their work
  outside it on purpose, `ami_sana2_close()`'s `ami_free()` included. Found while
  auditing the interface reads (see the header of `src/bsdsocket/interfaces.c`);
  the fix is to take `ami_ns_lock` across both functions, which nests cleanly
  because nothing holds the ThreadX baton when it is taken.
- **`AmiIfConfig.up` is write-only.** `config_parse.c` records `STATE=down` from
  `DEVS:NetInterfaces` and nothing ever reads it, so the interface comes up
  anyway.
- **`vsyslog` is `ENOSYS`.** `LOGSTAT`/`LOGMASK`/`LOGFACILITY`/`LOGTAGPTR` are
  stored and never read, which costs a caller nothing. Implementing syslog is
  the open part; the two tags below are not.
- **`SIOCGIFADDR` missing from `bpf_ioctl`.** Needs a setter, not a wider attach
  call — the address changes over a DHCP lease.
- **`bpf_read()` never blocks.** Gate on a non-zero `BIOCSRTIMEOUT`, as 4.4BSD
  does; `bpf_set_interrupt_mask` gives `EINTR` a producer. The wait must be on the
  *calling* task's signals — never a `MsgPort` made on another Process (544398f).
- **Multicast socket options absent** — `IP_MULTICAST_TTL/IF/LOOP`,
  `IP_ADD_MEMBERSHIP`, plus `IP_OPTIONS` and `IP_RECVDSTADDR`. Wanted eventually
  for Bonjour. Also `SO_RCVLOWAT`, which `recv()`'s documented behaviour depends
  on, and `SO_DEBUG`/`SO_DONTROUTE`/`SO_SNDLOWAT`.
- **`SBTC_IP_FILTER_HOOK` unserviced** — needs the `mbuf_*` family, which is why
  both are stubbed.
- **`AAMR_AddressInUse` / `AAMR_MaskChangeFailed` never produced**;
  `AAMP_BOOTP`/`SLOWAUTO`/`FASTAUTO` answer `AAMR_Ignored`.
- **`GetRouteInfo`** emits `rtm_index = 0` for static and default-gateway routes,
  and uses NetX's 0-based interface indices where BSD's are 1-based.
- **`IFQ_MaxReadRequests` / `MaxWriteRequests` unanswered.** The doc types them
  `(LONG)` where all 40 neighbours are `(LONG *)` — almost certainly a doc typo,
  and writing through a scalar would corrupt a caller.
- **The freeze is explained but not proven.** The scheduler-state defect (`4a1ad30`)
  accounts for the Enforcer hits; the link to the hang is reasoning. Never
  reproduced locally — our emulator cannot reach the packet rate.
- **We ingest 816 packets of 9.7M under saturation**, readers suspended (RESEARCH
  §79). Nothing is chasing it, and the figure should be re-measured before
  anything is built on it -- two confounds were found on 2026-07-31, both
  eliminable in one run:
  1. The run predates the baton fix (`4a1ad30` is not an ancestor of `4fb5eb2`),
     so it carried the defect that corrupted ThreadX suspension lists -- and the
     symptom recorded was suspended `sana2 rx` threads. The note's "baton
     counters were clean" does not exonerate it: those count slot occupancy, not
     the `_tx_thread_system_state` misuse `4a1ad30` fixed.
  2. Load came from playhouse2, whose TX checksums are uncomputed and still are.
     If the IP header checksum was among them, the flood was correctly dropped at
     IP and 816 is unrelated LAN traffic rather than a starvation figure.
  The rig survives at `/tmp/rig` + `/tmp/patch_rig.py` on playhouse3; the guest
  reporter is not in the repository. Re-run against playhouse4, not playhouse2.
- **`ShowNetServices` cannot browse every type at once.** With no type it runs the
  RFC 6763 §9 meta-query and lists the types present; listing every instance of
  every type would mean starting one continuous query per type found. They would
  all run concurrently, so it costs one more window rather than one per type — but
  it multiplies what lands in the peer cache, and the cache size was already the
  thing that decided whether an answer had an address in it or not.
- **A browse answer with no address is not chased.** When the PTR and SRV arrive
  without the A record in the same response, the row prints the target host and
  "no address" — the vendored module fills `service_ipv4` only from an A record
  already in the cache and never asks for one. Raising the peer cache to 32 KB
  made it rare on the LAN it was measured against, which is a mitigation rather
  than a fix: the right answer is to resolve the SRV target when the A is absent.
- **A browse reports the whole peer cache, not the browse window.**
  `netstack_mdns_browse_collect()` walks `nx_mdns_service_lookup()` by index,
  which is the whole peer cache, and nothing ages entries from our side -- the
  module expires them by TTL, and an unplugged machine sends none of RFC 6762
  §10.1's goodbyes. That part is mDNS working as designed and needs no fix.
  The claim it made was wrong and is fixed: the output said "what answered in
  the window", true on a cold cache and false on a warm one, and now says what
  the machine has heard recently and that a listing may have gone. What is left
  is the behaviour -- filter to entries actually refreshed inside the window.
  `nx_mdns_rr_elapsed_time` and `nx_mdns_rr_remaining_ticks` carry the
  freshness, but `NX_MDNS_SERVICE` does not, so it means walking the RR cache
  instead of the public lookup. Not worth that trade until someone reports a
  switched-off machine lingering, which every other mDNS browser does too.

## Decided against — do not "fix"

- **`SBTC_LOG_FILE_NAME` and `SBTC_LOG_HOOK` are refused**, 2026-07-31. The
  autodoc sanctions it in their own entries: "This tag is an extension to the
  AmiTCP V4 API and cannot be expected to be supported by older
  'bsdsocket.library' versions." Neither constant is in the NDK headers either,
  so a caller has to define it before it can pass one. Refusing costs a rare
  caller a tag list it was told to expect to lose.

- **`sendto()` with an address on a connected UDP socket.** Doc says `EISCONN`;
  everything portable dropped that rule.
- **UDP send with no destination returns `EDESTADDRREQ`**, not `-udp-`'s
  `ENOTCONN`. Ours is what 4.4BSD returns and what portable code checks.
- **`listen()` on an unbound socket fails** rather than auto-binding.
- **`MSG_OOB` refused by the msghdr forms**; `MSG_DONTROUTE` accepted and ignored.
- **`listen()` backlog is 8**, where the doc's BUGS claims a silent limit of 5.
- **`Dup2Socket` allows any in-range slot** — the doc's `EBADF` wording would make
  `dup2` onto a free slot illegal, defeating the call.
- **Request-count defaults** (ours sized from the packet pool, not 32/32/4).
- **`IP_DEFAULT_TTL` is 128**, doc says 64. Deliberate, 2026-07-31.
- **`gai_strerror()` takes its argument in A0.** The autodoc says D0; the SFD and
  pragmas say A0, and callers link against the pragma. The doc is wrong.
- **`bpf_open()` returns 0 for channel 0**, against the doc's *"handle > 0"* —
  RESEARCH §60 decoded Roadshow's own libpcap using it as a 0-based channel.
- **`OpenLibrary` allows Tasks and non-opener Processes**, which the doc denies.
  More permissive, never less.
- **`GetDefaultDomainName()` refuses rather than truncates.**
- **Packet pool sizing left alone**, 2026-07-31 — the heuristic reaches the floor
  on small machines (1 free of 17 observed on 1 MB), so the minimum must stay.
- **No stripped drawer beyond `68000-minimal`**, 2026-07-31 — the full build is
  what generates useful bug reports.
- **Two physical NICs**, dropped 2026-07-31: `romtype_restricted()` keeps only the
  first card, so it is untestable, and the complexity is not worth a rare case.
  Recovery SHA `d22a33e`.

- **A Kickstart 1.3 build**, dropped 2026-07-31: TheWire13 already covers that
  platform. Our side was closer than expected -- tag walking is hand-rolled so
  `utility.library` is never needed, the library's DOS surface is all V33, and
  the 68000 build ships -- but `ReadEClock` is V36 and everything 0.14.0 did for
  clock correctness rests on it; a 1.3 fallback is the 50 Hz CIA/VBlank source,
  exactly one tick of resolution, which retires the timer budget. The tools are
  the volume (`ReadArgs` 27 sites, `FreeArgs` 191, `VPrintf` 12). Never
  established whether any SANA-II driver runs under V34 at all, which was the
  gating question.

## Environment and tooling

- **`playhouse2` had uncomputed TX checksums**; `ethtool -K eth0 tx off` was applied
  2026-07-31 and verified (`/usr/sbin/ethtool -k eth0` -> `tx-checksumming: off`;
  `/usr/sbin` is not on a non-login ssh PATH, so query it by full path). Same defect
  playhouse4 had. `run-fitzbench.sh` still refuses it outright and can be relaxed.
- **`run-fitzbench.sh` prints a write figure that is not a rate** — it stops timing
  when the write call returns, not when data drains. Guest-timed 1718 KB/s against
  a measured wire rate of 364. Reads agree between clocks; writes diverge ~4.7x.
- **cppcheck**: baseline is from 2.20.0, gate hosts have 2.17.1, so the stage skips
  itself. Install 2.20.0 or regenerate to make it gate again.
- **FS-UAE cannot boot headless on playhouse3** (`FATAL: [GLAD] …`). Harnesses take
  `-A` to use Amiberry instead, and `-a ARGS` to pass arguments.
