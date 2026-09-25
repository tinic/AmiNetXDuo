# Project priorities

Decision snapshot from the 2026-09-24 project-wide review. This is a ranking of
possible work, not a list of known defects; the latter belongs in
[BACKLOG.md](../BACKLOG.md). Priority reflects user value, evidence and scope, not
an instruction to start every row. The Roadshow receive gap is the current
user-selected performance priority; the status-report tool has shipped.

| Project | Priority | Value | Smallest useful result |
|---|---|---|---|
| Roadshow receive gap on `x-surf-100.device` | **Now — user-selected performance priority** | High: the one recorded same-driver receive deficit against Roadshow (949 against 918 KB/s on 0.25.5, a 68060 A3000) | First establish whether a gap survives on a current pinned build, with one driver binary, one stack per boot, alternated order, a null control and an interval. Mechanism only after that. Protocol: [roadshow-rx-gap.md](roadshow-rx-gap.md) |
| `CreateAmiNetXDuoStatusReport` | **Shipped** (#31, #33) | High support value: one useful attachment instead of several rounds asking for versions, hardware and status | Passive, allowlisted, console plus `T:AmiNetXDuoStatusReport.txt`. Known follow-ups from its first real run: device names cut at 32 characters, empty interface slots printed |
| End-to-end release gate | **P0 — existing release-safety work** | Very high: prevents a tagged build from bypassing emulator end-to-end coverage | A release refuses a tag SHA without a successful, relevant emulator run on that SHA. Keep this separate from the new-feature ranking. |
| Real-application compatibility matrix | **P1 — first new initiative after current work** | Very high direct user value: answers whether the applications people use actually work, beyond socket conformance | Publish pinned versions and results for `smb2fs`, NetSurf with AmiSSL, and WookieChat on one fixed emulator configuration. Use Roadshow as a control when attributing a failure. Start exploratory; promote only reproducible, scriptable cases to a release smoke gate. An ixemul-built, no-FPU `wget` is a fourth legacy-compatibility probe. |
| Roadshow migration report | **P1** | High adoption value: makes copied configurations and scripts predictable instead of silently ignoring settings | Read representative Roadshow configurations and classify each setting as applied, ignored, unsupported or behaviorally different. Implement individual compatibility changes only where a real workflow needs them. |
| Clockless-machine TLS policy | **P2** | Medium, targeted value: clearer and partly safer behavior when a machine has no trustworthy clock | Document and expose whether certificate dates were checked. Investigate a deterministic build-date, `notAfter`-only lower bound with a distinct result state; it is **not** full date validation. Unauthenticated DHCP/NTP time must not be represented as trusted. |
| 040/060 hardware doctor | **P2, conditional** | Medium for affected owners: turns “card detected, no traffic” into a useful support report | Add only verified CPU/cache/board-mapping facts to passive diagnostics; report `unknown` otherwise. Raise priority if a current 040/060 report or reproducible hardware result warrants it. This is not a presumed fix for the unreplicated 68060 collapse. |
| Explicit WebDAV discovery | **P3** | Modest: makes an intentionally configured share easier to find | Advertise the service only when a share root is explicitly configured. Do not create a default share or reopen the previously rejected authentication redesign. |
| NAT | **Deferred** | Demand unproven against large implementation and security cost | Require a concrete current use case before design work. PPP/SLIP remains excluded by the earlier project decision. |

## How to keep the app matrix honest

The initial matrix is a *user-visible compatibility report*, not automatically
a CI gate. Pin application archives, server versions, stack SHA, OS, CPU and
emulated card. A matching Roadshow run on the same setup helps distinguish an
application or rig failure from a stack failure. NetSurf exercises a real
browser and AmiSSL, but its rendering and website dependencies make it a poor
gate until a fixed local page and server make the result repeatable. WookieChat
DCC remains exploratory; its ordinary connect/join/message/idle-resume loop
could become a smoke case only if documented scriptable controls exist.

## Status-report safety boundary

The report is a new *support workflow*, largely assembling information already
available through separate tools. Existing diagnostic commands are candidate
data sources, not automatically safe commands for the report to execute. In
particular, ordinary `CheckNetDevice` can load a driver; the report may read a
published resident probe record or use proven `NOLOAD` behavior, but must never
take over a card. Prefer an explicit field allowlist over dumping configuration
files and trying to scrub them afterward. The report stays local and text-only.
