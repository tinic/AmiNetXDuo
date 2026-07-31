# Backlog

What is outstanding, what was decided against, and why. Findings come mostly from
the NDK 3.2 autodoc audit (four passes over all 122 documented entries) and from
the memory-floor and stress work.

The autodoc is at `NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc`. **`grep` silently
fails on it** — `file` misidentifies it as "GTA in-game text". Read it with python.

---

## Open — no decision taken

- **`SM_Online` failure leaves earlier tags applied.** Doc: *"if it fails … no
  further configuration will have been done."* We apply `IFC_LimitMTU`, then
  address/mask, then state, so an `S2_ONLINE` failure returns `ENXIO` with the MTU
  and address already changed.
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
- **`vsyslog` is `ENOSYS`**, so `SBTC_LOG_FILE_NAME` and `SBTC_LOG_HOOK` are
  unserviced and poison tag lists. `LOGSTAT`/`LOGMASK`/`LOGFACILITY`/`LOGTAGPTR`
  are stored and never read.
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
- **We ingest 816 packets of 9.7M under saturation**, readers suspended. Found by
  the flood rig; nothing is chasing it.
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
- **Nothing ages entries out of the mDNS peer cache from our side.** A service
  that has gone away stays listed until its TTL expires or the cache evicts it,
  so two browses a minute apart can report a machine that has since been switched
  off. RFC 6762 §10.1 goodbye packets are honoured by the module; a machine that
  is unplugged sends none.

## Decided against — do not "fix"

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

- **`playhouse2` is unusable as a network peer** until `ethtool -K <iface> tx off`.
  Same defect as playhouse4 had; `run-fitzbench.sh` now refuses it outright.
- **`run-fitzbench.sh` prints a write figure that is not a rate** — it stops timing
  when the write call returns, not when data drains. Guest-timed 1718 KB/s against
  a measured wire rate of 364. Reads agree between clocks; writes diverge ~4.7x.
- **cppcheck**: baseline is from 2.20.0, gate hosts have 2.17.1, so the stage skips
  itself. Install 2.20.0 or regenerate to make it gate again.
- **FS-UAE cannot boot headless on playhouse3** (`FATAL: [GLAD] …`). Harnesses take
  `-A` to use Amiberry instead, and `-a ARGS` to pass arguments.
