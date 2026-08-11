# What the other stacks have and we do not

Surveyed 2026-08-11 against the two references we can read:

* **Roadshow 1.15**, from the SDK on the asset store rather than from prose:
  `NDK3.2/SANA+RoadshowTCP-IP/sfd/bsdsocket_lib.sfd` (125 entries),
  `netinclude/bsdsocket/socketbasetags.h`, and
  `Roadshow-Demo-1.15/Documentation`.
* **AmiTCP_NG 4.1.5-beta**, from source: `src/netinclude/fd/socket_lib.fd`,
  `src/api/`, `src/kern/`, `src/tools/`.

It exists because `NetShutdown` shipped for months doing a third of what the
name promises, and nobody had compared the whole surface. Rows are facts, not
plans: what is missing, where the evidence is, and what a user loses. Deciding
which to build is a separate question, and "never" is a legitimate answer —
`docs/BACKLOG.md` is where a decided one goes.

## Not gaps

Checked first, because most of the surface is already there and a survey that
only lists holes misleads.

| Surface | State |
|---|---|
| `bsdsocket.library` socket API | AmiTCP's `socket_lib.fd` is 45 entries and we implement all of them. Roadshow's sfd is 125 entries, 10 of which are varargs aliases; the 32 names we do not implement are listed below, and every one of them is outside the socket API proper |
| `usergroup.library` | All 39 vectors (`src/usergroup/`), same set as AmiTCP's `usergroup_lib.fd` |
| ARexx `AMITCP` port | We have AmiTCP 3.0b2's whole keyword set, `Q=QUERY,S=SET,READ,ROUTE,ADD,RESET,KILL` (`netstack_rexx.c:61`). AmiTCP_NG implements `KILL` alone |
| BPF / packet capture | `bpf_*`, 8 vectors, plus `NetTrace` |
| `TCP:` handler | `src/bsdsocket/tcp_handler.c`, `TCPHANDLER=` in the interface file |
| Interface, routing, monitoring, status, DNS, local-database, address-conversion APIs | Implemented, and `SBTC_HAVE_*` says so truthfully (`errno.c:488-536`) |

## Missing vectors

From the sfd diff. Varargs aliases (`AddInterfaceTags` for
`AddInterfaceTagList`, and nine more) are excluded — they are stub-level, not
LVOs.

| Vector | LVO | Ours | What a user loses |
|---|---|---|---|
| `ObtainRoadshowData` `ReleaseRoadshowData` `ChangeRoadshowData` | sfd 159-161 | `bsd_enosys` | The tunables API. This is what `RoadshowControl` drives; without it there is no way to read or set `tcp.sendspace`, `ip.forwarding`, `icmp.processecho` and the rest at runtime. `SBTC_HAVE_ROADSHOWDATA_API` correctly answers FALSE |
| `ChangeRouteTagList` | -0x1aa (`bsdsocket_vectors.c:99`) | `bsd_enosys` | A route can be added and deleted but not modified. Changing a metric or gateway means delete-then-add, which drops traffic in between |
| `ipf_open` `ipf_close` `ipf_ioctl` `ipf_log_read` `ipf_log_data_waiting` `ipf_set_notify_mask` `ipf_set_interrupt_mask` | sfd 172-178 | absent | The IP filter and NAT API. Roadshow ships `ipf`, `ipfstat`, `ipnat`, `ipmon` and `S:IPF` rules on top of it |
| `mbuf_get` `mbuf_gethdr` `mbuf_free` `mbuf_freem` `mbuf_copym` `mbuf_copydata` `mbuf_copyback` `mbuf_cat` `mbuf_adj` `mbuf_prepend` `mbuf_pullup` | sfd 146-157 | absent | The kernel memory API. A program that walks the stack's own buffers cannot. `SBTC_HAVE_KERNEL_MEMORY_API` correctly answers FALSE. NetX Duo has `NX_PACKET`, not mbufs, so this is a translation layer rather than an omission |
| `syslog` `vsyslog` | -0x0fc, -0x102 (`bsdsocket_vectors.c:71`) | `bsd_enosys` | `SyslogA` is in AmiTCP's fd and in ours as a stub. A program that logs through the stack logs nothing |
| `ProcessIsServer` `ObtainServerSocket` | -0x2b2, -0x2b8 (`bsdsocket_vectors.c:143`) | present but `SBTC_HAVE_SERVER_API` is FALSE | Worth resolving one way or the other: two vectors that exist beside a flag saying they do not |

## Missing SocketBaseTagList tags

**Three**, against the authoritative header
(`NDK3.2/SANA+RoadshowTCP-IP/netinclude/libraries/bsdsocket.h`, 53 tags; we
answer 51).

| Tag | What a user loses |
|---|---|
| `SBTC_LOG_FILE_NAME` `SBTC_LOG_HOOK` | Where the stack's own messages go. This is the mechanism behind Roadshow's `NetLogViewer`: a program cannot redirect the log or catch it |
| `SBTC_IP_FILTER_HOOK` | The hook the IP filter installs. Goes with the `ipf_*` vectors above |

AmiTCP_NG's source carries **34 further tags that Roadshow does not define** —
`SBTC_TPM_*` (23), `SBTC_SOWK_*` (5), `SBTC_TCP_SENDSPACE`/`_RECVSPACE`/
`_PCBMISS`/`_PRED*`/`_RCVTOTAL`, `SBTC_SB_MAX`, `SBTC_HOSTID`,
`SBTC_LINK_SPEED`, `SBTC_DETECTED_RAM`, `SBTC_LOG`, `SBTC_COMPAT43`. They are
AmiTCP 4.x and Miami era. Two are worth knowing about:

* `SBTC_COMPAT43` — 4.3BSD compatibility mode, which changes what `accept()`
  and `recvfrom()` write into a `sockaddr`. Software old enough to ask for it
  gets nothing from us, and gets nothing from Roadshow either.
* `SBTC_TPM_*` and `SBTC_SOWK_*` — TCP and socket-wakeup counters. Roadshow
  reports the equivalent through `NETSTATUS`-style calls instead; we have
  `NETSTATUS_STATS`. Not a gap so much as a different shape.

Whether to answer any of these is a compatibility question about AmiTCP-era
software, not a Roadshow one.

## Missing configuration

`DEVS:NetInterfaces/<name>`. We act on `DEVICE`, `UNIT`, `ID`, `CONFIGURE`,
`CONFIGURE6`, `IPTYPE`, `ADDRESS`, `ADDRESS6`, `NETMASK`, `GATEWAY`,
`GATEWAY6`, `MTU`, `STATE`, `MDNS`, and the AmiTCP spellings of four of them.

**The other 22 are accepted and silently dropped.** `config_parse.c:104-130`
maps them to `IF_KEY_IGNORED` so a stock Roadshow file produces no warnings —
which means a user who writes one gets no error, no effect, and nothing to
read. That is a worse failure than refusing the file, and it applies to all of
these:

| Key | What it would do |
|---|---|
| `REQUIRESINITDELAY` | A second's pause after opening a device that needs one. The manual names the original Ariadne — a card we have had trouble with |
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

Two answers are defensible for each — implement it, or say in one line why it
is refused — and "accepted, ignored, silent" is neither.

`DEVS:Internet/`: we read `hosts`, `networks`, `protocols`, `services`,
`routes`, `name_resolution`, plus our own `certificates`,
`service_discovery`, `tcp_handler`, `tlssessions`.

| File | Note |
|---|---|
| `users`, `groups` | **We read `passwd` and `group` instead** (`ug_db.c:31-34`), which are AmiTCP's names. Roadshow's manual §2675 and §3371 call them `users` and `groups`. Its own words are that each "uses a different format" from the Unix file, so this is two questions, not one: the name, and whether the contents would parse if it were renamed. Neither has been checked against a real Roadshow install. The README says we read the same configuration files Roadshow does |
| `rpc` | RPC program numbers, `getrpcbyname()`. Niche |
| `servers` | The inetd-style superserver table. Out of scope while we ship no daemons, and `docs/BACKLOG.md` should say so rather than leaving it looking forgotten |

## Missing commands

`ftp`, `ftpd`, `telnetd`, `rsh` and an ssh server are **not** in this table
and are not gaps. They are decided against, in both directions, and "the other
stacks ship one" is not an argument for any of them: WebDAV over `httpd`
covers moving files, `telnet` and `ssh` the clients cover the rest, and
`docs/BACKLOG.md` records the decision. Nor is anything here about PPP, PPPoE,
SLIP or a modem.

| Command | Theirs | What a user loses |
|---|---|---|
| `RoadshowControl` | Roadshow | Reading and setting the tunables above, and `ENV:Roadshow/<group>/<name>` so they survive a reboot. Needs the RoadshowData vectors first |
| `ManageNetInterfaces` | Roadshow | Moving interface files between `DEVS:NetInterfaces` and `SYS:Storage/NetInterfaces` so a card that is not present does not fail at boot |
| `SampleNetSpeed` | Roadshow | A window showing throughput per interface |
| `NetLogViewer` | Roadshow | A commodity that catches what the stack and its clients log |
| `ipf` `ipfstat` `ipnat` `ipmon` | Roadshow | Packet filtering and NAT |
| `CheckRoadshowConfig` | Roadshow | We have `CheckNetConfig`, which is the same idea under our name |
| `wget`, `tcpdump` | Roadshow | We have `fetch` and `NetTrace` |

## Behaviour, not surface

| Item | Evidence |
|---|---|
| Internationalised domain names | Roadshow translates a Latin-1 domain name to Punycode transparently (manual §73, `SBTC_IDN_DEFAULT_CHARACTER_SET`). We have no Punycode anywhere |
| ARexx `KILL` | `netstack_rexx.c:153` takes the interfaces down. Now that `NETCTRL_STACK_NOTIFY` and `_RELEASE` exist it should do what `NetShutdown` does, or the ARexx path is the old third-of-the-job |
