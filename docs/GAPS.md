# What the other stacks have and we do not

Facts, not plans: what is missing, where the evidence is, and what a user loses.
"never" is a legitimate answer, and `docs/BACKLOG.md` is where a decided one
goes. Compared against **Roadshow 1.15** (`NDK3.2/SANA+RoadshowTCP-IP/sfd/bsdsocket_lib.sfd`,
125 entries; `netinclude/libraries/bsdsocket.h`, 53 tags) and **AmiTCP_NG
4.1.7** (`src/netinclude/fd/socket_lib.fd`, 45 entries; `src/api/`, `src/kern/`,
`src/tools/`).

## Not gaps

| Surface | State |
|---|---|
| `bsdsocket.library` socket API | AmiTCP's `socket_lib.fd` is 45 entries and we implement all of them. Roadshow's sfd is 125, 10 of which are varargs aliases; the 31 names we do not implement are below, and every one is outside the socket API proper |
| `usergroup.library` | all 39 vectors (`src/usergroup/`), the same set as AmiTCP's `usergroup_lib.fd` |
| BPF / packet capture | `bpf_*`, 8 vectors, plus `NetCapture` and `NetTrace` |
| `TCP:` handler | `src/bsdsocket/tcp_handler.c`, `TCPHANDLER=` in the interface file |
| `syslog` / `vsyslog` | AmiTCP's `syslog()` inline forwards its packed argument stream to the `vsyslog` LVO. We honor the opener's tag, `LOG_PID` and mask, expand `%m`, accept facility bits, and emit through the serial diagnostic sink |
| Interface, routing, monitoring, status, DNS, local-database, address-conversion APIs | implemented, and `SBTC_HAVE_*` says so truthfully (`errno.c:488-536`) |
| Interface files | named by path (`AddNetInterface Work:weth0`), else `DEVS:NetInterfaces` then `SYS:Storage/NetInterfaces`; a name with a device or directory in it is looked for nowhere else. `NAMESERVER` and `DOMAIN` in one are read as the last resolver source, with a note (`config_list.c`) |
| `ConfigureNetInterface` `MTU` `ONLINE` `OFFLINE` `UP` `DOWN`, `ShowNetStatus` `IGMP` `MULTICASTROUTING` `ROUTING` `QUIET` | taken, through `ConfigureInterfaceTagList()` and `NETSTATUS_MULTICAST` |

## Missing vectors

From the sfd diff. Varargs aliases (`AddInterfaceTags` for `AddInterfaceTagList`,
and nine more) are excluded: they are stub-level, not LVOs.

| Vector | LVO | Ours | What a user loses |
|---|---|---|---|
| `ObtainRoadshowData` `ReleaseRoadshowData` `ChangeRoadshowData` | sfd 159-161 | `bsd_enosys` | The tunables API, which is what `RoadshowControl` drives. No way to read or set `tcp.sendspace`, `ip.forwarding`, `icmp.processecho` and the rest at runtime. `SBTC_HAVE_ROADSHOWDATA_API` correctly answers FALSE |
| `ipf_open` `ipf_close` `ipf_ioctl` `ipf_log_read` `ipf_log_data_waiting` `ipf_set_notify_mask` `ipf_set_interrupt_mask` | sfd 172-178 | absent | The IP filter and NAT API. Roadshow ships `ipf`, `ipfstat`, `ipnat`, `ipmon` and `S:IPF` rules on top of it |
| `mbuf_get` `mbuf_gethdr` `mbuf_free` `mbuf_freem` `mbuf_copym` `mbuf_copydata` `mbuf_copyback` `mbuf_cat` `mbuf_adj` `mbuf_prepend` `mbuf_pullup` | sfd 146-157 | absent | The kernel memory API: a program that walks the stack's own buffers cannot. `SBTC_HAVE_KERNEL_MEMORY_API` correctly answers FALSE. NetX Duo has `NX_PACKET`, not mbufs, so this is a translation layer rather than an omission |
| `ProcessIsServer` `ObtainServerSocket` | -0x2b2, -0x2b8 (`bsdsocket_vectors.c:143`) | present, but `SBTC_HAVE_SERVER_API` is FALSE | Two vectors that exist beside a flag saying they do not. Worth resolving in one direction or the other |

## Missing SocketBaseTagList tags

Three, of Roadshow's 52 codes (53 defines, one a macro: `SBTC_ERRNOPTR(size)`
picks among the three `ERRNO*PTR` codes we implement). We answer 49, counted by
`tools/check-sbtc-tags.sh` rather than asserted here.

| Tag | What a user loses |
|---|---|
| `SBTC_LOG_FILE_NAME` `SBTC_LOG_HOOK` | Where the stack's own messages go. This is the mechanism behind Roadshow's `NetLogViewer`: a program cannot redirect the log or catch it |
| `SBTC_IP_FILTER_HOOK` | The hook the IP filter installs. Goes with the `ipf_*` vectors |

`SBTC_CAN_SHARE_LIBRARY_BASES` is an application **opt-in**, not a capability
advert: AmiTCP_NG's `socketbasetags.h` has it as "Roadshow's opt-in to sharing
one library base between tasks", and `CHECK_TASK()` refuses a non-opener caller
until it is set. **We enforce no same-task rule at all**, so the restriction it
relaxes does not exist here and a SET is accepted rather than refused. What
sharing does not buy is what Roadshow documents — signals go to the opener,
`errno` is per base — plus one of ours: `WaitSelect()`'s timer is served by the
first task that asks for a timeout, and a second gets `EINVAL`. Per-task timer
state is reopening the library, which Roadshow's autodoc recommends anyway.
Pinned by `run-ifquery.sh`, including a second task's timed `WaitSelect`.

AmiTCP_NG carries 34 further tags Roadshow does not define — `SBTC_TPM_*` (23),
`SBTC_SOWK_*` (5), the `SBTC_TCP_*` counters, `SBTC_SB_MAX`, `SBTC_HOSTID`,
`SBTC_LINK_SPEED`, `SBTC_DETECTED_RAM`, `SBTC_LOG`, `SBTC_COMPAT43`. AmiTCP 4.x
and Miami era; Roadshow answers none of them either, and the `TPM`/`SOWK`
counters are reported in another shape through `NETSTATUS_STATS`.

## ARexx: parsed but refused

`netstack_rexx.c:61` declares AmiTCP 3.0b2's whole keyword set,
`Q=QUERY,S=SET,READ,ROUTE,ADD,RESET,KILL`. AmiTCP_NG implements `KILL` alone.

| Keyword | State |
|---|---|
| `QUERY` `SET` `KILL` | implemented, `:250-258`. `KILL` takes the interfaces down; with `NETCTRL_STACK_NOTIFY` and `_RELEASE` it should do what `NetShutdown` does (`:153`) |
| `READ` `ROUTE` `ADD` `RESET` | recognised, refused at `:273-279`. `ADD` and `RESET` need AmiTCP's mutable net database; `netdb.c` is immutable after `ami_netdb_load()`, which is why it needs no lock |

## Missing configuration

`DEVS:NetInterfaces/<name>`. The 19 keys acted on are listed at
`src/config/config_parse.c:78`, with the AmiTCP spellings of four of them.
`ADDRESS6` takes two lines per interface; every other key takes one.

**The other 21 are accepted and silently dropped**: `config_parse.c:78` maps
them to `IF_KEY_IGNORED` so a stock Roadshow file produces no warnings, which
means a user who writes one gets no error, no effect and nothing to read. Two
answers are defensible for each — implement it, or refuse it in one line.

| Key | What it would do |
|---|---|
| `ALIAS` | A second address on one interface |
| `COPYMODE` | Which SANA-II copy mode the driver is asked for |
| `IPREQUESTS` `WRITEREQUESTS` `ARPREQUESTS` | How many requests are queued to the driver at once — the throughput knob for a slow card |
| `POINTTOPOINT` `DESTINATION` | Point-to-point links |
| `MULTICAST` | Asking the driver for multicast explicitly |
| `REPORTOFFLINE` | Whether an interface going offline is reported |
| `METRIC` `PRIORITY`/`PRI` | Ordering two interfaces |
| `LEASE` `DHCPUNICAST` | DHCP lease time and unicast renewal (`ID` we do read) |
| `BROADCASTADDRESS` | A broadcast address other than the one the netmask implies |
| `FILTER` `DEBUG` `ARPTYPE`/`HARDWARETYPE` `LINKSTATUSCOMMAND` | Packet filter, driver debug, ARP hardware type, link-change command |

Roadshow's `ConfigureNetInterface` takes one keyword per interface-file key, so
that list is also what it cannot be asked for at runtime.

`DEVS:Internet/`: `hosts`, `networks`, `protocols`, `services`, `routes`,
`name_resolution`, plus our own `certificates`, `service_discovery`,
`tcp_handler` and `tlssessions`.

| File | Note |
|---|---|
| `users`, `groups` | **We read `passwd` and `group` instead** (`ug_db.c:32-41`), AmiTCP's names, and `ug_parse.c:95` already takes `\|` or `:` per record, so the separator was never the gap. Roadshow's are ReadArgs lines, from its own manuscript: `users` is `NAME/A,PASSWORD/K,UID/A/N,GID/A/N,GECOS,DIR,SHELL` and `groups` is `NAME/A,ID/A/N,USERS/M`, `#` comments and blank lines skipped, **password in plain text**. Roadshow's manual calls both files obsolete, "provided only in order to assist the few applications which require them", and recommends against putting login data in them — so this is a small parser for a facility its own author deprecates, not a missing subsystem |
| `rpc` | RPC program numbers, `getrpcbyname()`. Niche |
| `servers` | The inetd-style superserver table. Out of scope while we ship no daemons |

## Missing commands

`ftp`, `ftpd`, `telnetd`, `rsh` and an ssh server are **not** gaps and are not
in this table: they are decided against in `docs/BACKLOG.md`, as is anything to
do with PPP, PPPoE, SLIP or a modem.

| Command | Theirs | What a user loses |
|---|---|---|
| `RoadshowControl` | Roadshow | The tunables above, and `ENV:Roadshow/<group>/<name>` so they survive a reboot. Needs the RoadshowData vectors first |
| `ManageNetInterfaces` | Roadshow | Moving interface files between `DEVS:NetInterfaces` and `SYS:Storage/NetInterfaces`. Both drawers are searched for a bare name, so a file in either one comes up when it is named; what is missing is the command that moves it |
| `SampleNetSpeed`, `NetLogViewer` | Roadshow | A throughput window per interface, and a commodity that catches what the stack and its clients log |
| `ipf` `ipfstat` `ipnat` `ipmon` | Roadshow | Packet filtering and NAT, on the `ipf_*` vectors above |
| `CheckRoadshowConfig`, `wget`, `tcpdump` | Roadshow | We have `CheckNetConfig`, `fetch` and `NetCapture` under our own names |

## Behaviour, not surface

**Internationalised domain names.** Roadshow translates Latin-1 to Punycode
transparently (manual §73, `SBTC_IDN_DEFAULT_CHARACTER_SET`); we have none.
