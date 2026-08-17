# What the other stacks have and we do not

Facts, not plans. What is missing, where the evidence is, and what a user loses.
Deciding which to build is a separate question and "never" is a legitimate
answer; `docs/BACKLOG.md` is where a decided one goes.

Compared against **Roadshow 1.15** (`NDK3.2/SANA+RoadshowTCP-IP/sfd/bsdsocket_lib.sfd`,
125 entries; `netinclude/libraries/bsdsocket.h`, 53 tags; `Roadshow-Demo-1.15/Documentation`)
and **AmiTCP_NG 4.1.5-beta** (`src/netinclude/fd/socket_lib.fd`, `src/api/`,
`src/kern/`, `src/tools/`).

## Not gaps

| Surface | State |
|---|---|
| `bsdsocket.library` socket API | AmiTCP's `socket_lib.fd` is 45 entries and we implement all of them. Roadshow's sfd is 125, 10 of which are varargs aliases; the 31 names we do not implement are below, and every one is outside the socket API proper |
| `usergroup.library` | all 39 vectors (`src/usergroup/`), the same set as AmiTCP's `usergroup_lib.fd` |
| BPF / packet capture | `bpf_*`, 8 vectors, plus `NetCapture` and `NetTrace` |
| `TCP:` handler | `src/bsdsocket/tcp_handler.c`, `TCPHANDLER=` in the interface file |
| Interface, routing, monitoring, status, DNS, local-database, address-conversion APIs | implemented, and `SBTC_HAVE_*` says so truthfully (`errno.c:488-536`) |

## Missing vectors

From the sfd diff. Varargs aliases (`AddInterfaceTags` for `AddInterfaceTagList`,
and nine more) are excluded: they are stub-level, not LVOs.

| Vector | LVO | Ours | What a user loses |
|---|---|---|---|
| `ObtainRoadshowData` `ReleaseRoadshowData` `ChangeRoadshowData` | sfd 159-161 | `bsd_enosys` | The tunables API, which is what `RoadshowControl` drives. No way to read or set `tcp.sendspace`, `ip.forwarding`, `icmp.processecho` and the rest at runtime. `SBTC_HAVE_ROADSHOWDATA_API` correctly answers FALSE |
| `ipf_open` `ipf_close` `ipf_ioctl` `ipf_log_read` `ipf_log_data_waiting` `ipf_set_notify_mask` `ipf_set_interrupt_mask` | sfd 172-178 | absent | The IP filter and NAT API. Roadshow ships `ipf`, `ipfstat`, `ipnat`, `ipmon` and `S:IPF` rules on top of it |
| `mbuf_get` `mbuf_gethdr` `mbuf_free` `mbuf_freem` `mbuf_copym` `mbuf_copydata` `mbuf_copyback` `mbuf_cat` `mbuf_adj` `mbuf_prepend` `mbuf_pullup` | sfd 146-157 | absent | The kernel memory API: a program that walks the stack's own buffers cannot. `SBTC_HAVE_KERNEL_MEMORY_API` correctly answers FALSE. NetX Duo has `NX_PACKET`, not mbufs, so this is a translation layer rather than an omission |
| `syslog` `vsyslog` | -0x0fc, -0x102 (`bsdsocket_vectors.c:71`) | `bsd_enosys` | `SyslogA` is in AmiTCP's fd and in ours as a stub. A program that logs through the stack logs nothing |
| `ProcessIsServer` `ObtainServerSocket` | -0x2b2, -0x2b8 (`bsdsocket_vectors.c:143`) | present, but `SBTC_HAVE_SERVER_API` is FALSE | Two vectors that exist beside a flag saying they do not. Worth resolving in one direction or the other |

## Missing SocketBaseTagList tags

Three, against Roadshow's 53; we answer 51.

| Tag | What a user loses |
|---|---|
| `SBTC_LOG_FILE_NAME` `SBTC_LOG_HOOK` | Where the stack's own messages go. This is the mechanism behind Roadshow's `NetLogViewer`: a program cannot redirect the log or catch it |
| `SBTC_IP_FILTER_HOOK` | The hook the IP filter installs. Goes with the `ipf_*` vectors |

AmiTCP_NG's source carries 34 further tags Roadshow does not define —
`SBTC_TPM_*` (23), `SBTC_SOWK_*` (5), the `SBTC_TCP_*` counters, `SBTC_SB_MAX`,
`SBTC_HOSTID`, `SBTC_LINK_SPEED`, `SBTC_DETECTED_RAM`, `SBTC_LOG`,
`SBTC_COMPAT43`. AmiTCP 4.x and Miami era, so answering any of them is a
compatibility question about AmiTCP-era software, not a Roadshow one. Roadshow
answers none of them either. The `TPM`/`SOWK` counters we report in a different
shape, through `NETSTATUS_STATS`.

## ARexx: parsed but refused

`netstack_rexx.c:61` declares AmiTCP 3.0b2's whole keyword set,
`Q=QUERY,S=SET,READ,ROUTE,ADD,RESET,KILL`. Only QUERY, SET and KILL are
implemented (`:250-258`); READ, ROUTE, ADD and RESET are recognised and refused
with a "not implemented" error (`:273-279`). ADD and RESET would need AmiTCP's
mutable in-memory net database, and `src/config/netdb.c` is immutable after
`ami_netdb_load()`, which is why it needs no lock. AmiTCP_NG implements `KILL`
alone.

`netstack_rexx.c:153` — `KILL` takes the interfaces down. Now that
`NETCTRL_STACK_NOTIFY` and `_RELEASE` exist it should do what `NetShutdown`
does, or the ARexx path stays a third of the job.

## Missing configuration

`DEVS:NetInterfaces/<name>`. We act on `DEVICE`, `UNIT`, `ID`, `CONFIGURE`,
`CONFIGURE6`, `IPTYPE`, `ADDRESS`, `ADDRESS6`, `NETMASK`, `GATEWAY`,
`GATEWAY6`, `MTU`, `STATE`, `MDNS`, and the AmiTCP spellings of four of them.

**The other 22 are accepted and silently dropped**: `config_parse.c` maps them
to `IF_KEY_IGNORED` so a stock Roadshow file produces no warnings, which means a
user who writes one gets no error, no effect, and nothing to read. Two answers
are defensible for each — implement it, or refuse it in one line — and
"accepted, ignored, silent" is neither.

| Key | What it would do |
|---|---|
| `REQUIRESINITDELAY` | A second's pause after opening a device that needs one. The manual names the original Ariadne, a card we have had trouble with |
| `HARDWAREADDRESS` | Set the MAC. Two guests on one bridge collide without it, and so do two real cards sharing an address |
| `ALIAS` | A second address on one interface |
| `COPYMODE` | Which SANA-II copy mode the driver is asked for |
| `IPREQUESTS` `WRITEREQUESTS` `ARPREQUESTS` | How many requests are queued to the driver at once — the throughput knob for a slow card |
| `POINTTOPOINT` `DESTINATION` | Point-to-point links |
| `MULTICAST` | Asking the driver for multicast explicitly |
| `DOWNGOESOFFLINE` `REPORTOFFLINE` | Whether taking an interface down sends `S2_OFFLINE`, and whether that is reported |
| `METRIC` `PRIORITY`/`PRI` | Ordering two interfaces |
| `LEASE` `DHCPUNICAST` | DHCP lease time and unicast renewal (`ID` we do read) |
| `FILTER` `DEBUG` `ARPTYPE` `LINKSTATUSCOMMAND` | Packet filter, driver debug, ARP hardware type, link-change command |

`DEVS:Internet/`: we read `hosts`, `networks`, `protocols`, `services`,
`routes`, `name_resolution`, plus our own `certificates`, `service_discovery`,
`tcp_handler` and `tlssessions`.

| File | Note |
|---|---|
| `users`, `groups` | **We read `passwd` and `group` instead** (`ug_db.c:31-40`), which are AmiTCP's names. Roadshow's manual §2675 and §3371 call them `users` and `groups`, and says each "uses a different format" from the Unix file — so this is two questions, the name and whether the contents would parse if renamed. Neither has been checked against a real Roadshow install, and `README.md` claims we read the same configuration files Roadshow does |
| `rpc` | RPC program numbers, `getrpcbyname()`. Niche |
| `servers` | The inetd-style superserver table. Out of scope while we ship no daemons, and `docs/BACKLOG.md` should say so rather than leave it looking forgotten |

## Missing commands

`ftp`, `ftpd`, `telnetd`, `rsh` and an ssh server are **not** gaps and are not
in this table: they are decided against in `docs/BACKLOG.md`, as is anything to
do with PPP, PPPoE, SLIP or a modem.

| Command | Theirs | What a user loses |
|---|---|---|
| `RoadshowControl` | Roadshow | Reading and setting the tunables above, and `ENV:Roadshow/<group>/<name>` so they survive a reboot. Needs the RoadshowData vectors first |
| `ManageNetInterfaces` | Roadshow | Moving interface files between `DEVS:NetInterfaces` and `SYS:Storage/NetInterfaces` so a card that is not present does not fail at boot |
| `SampleNetSpeed` | Roadshow | A window showing throughput per interface |
| `NetLogViewer` | Roadshow | A commodity that catches what the stack and its clients log |
| `ipf` `ipfstat` `ipnat` `ipmon` | Roadshow | Packet filtering and NAT |
| `CheckRoadshowConfig` | Roadshow | We have `CheckNetConfig`, the same idea under our name |
| `wget`, `tcpdump` | Roadshow | We have `fetch` and `NetCapture` |

## Behaviour, not surface

| Item | Evidence |
|---|---|
| Internationalised domain names | Roadshow translates a Latin-1 domain name to Punycode transparently (manual §73, `SBTC_IDN_DEFAULT_CHARACTER_SET`). We have no Punycode anywhere |
