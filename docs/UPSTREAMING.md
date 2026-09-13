# Upstreaming the NetX Duo and ThreadX forks

Measured 2026-09-13 against `upstream/dev` (netxduo `082f3866`, threadx
`dde43b8a`). `tools/upstream-drift.sh --fetch` recomputes section 1.
Upstream's work is on `dev`; both forks already hold master's tip, so a
comparison against master reports no drift, which is false.

Not a plan of record. The scope decision — how much of this belongs in NetX
Duo at all — has not been taken.

## 1. What diverges

| | files | + | − |
|---|---|---|---|
| threadx | 6 | 85 | 42 |
| netxduo | 261 | 23,659 | 2,579 |
| ├ shipping code | 169 | 16,897 | 2,036 |
| │  ├ new files, ours | 25 | 6,892 | — |
| │  └ edits to upstream | 144 | 10,005 | 2,036 |
| └ tests | 92 | 6,762 | 543 |

162 of the 169 shipping files are code we compile. `common` 108 (+10,586),
`nx_secure` 39 (+3,148), `addons` 16 (+2,842), `crypto_libraries` 6 (+321).
Heaviest: `nxd_mdns.c` +1274, `nxd_dns.c` +649, `nx_tcp_socket_retransmit.c`
+591, **`nx_api.h` +554 −3**, `nxd_dhcpv6_client.c` +523, `nx_secure_x509.c`
+456. `nx_api.h` is public API we added and most TCP feature work depends on
it, so it gates whatever needs it.

## 2. What upstream accepts (last 26 merged PRs on dev)

Size is not the gate: they merged `+3956` (#392) and `+1844` (#399) in the
same window as `+2 −1` (#413). **One concern per PR is the gate** — not one of
the 26 bundles two fixes.

Every fix carries a regression test, and the test is the larger half:

| PR | source | test |
|---|---|---|
| TLS 1.3 de-padding #401 | +80 | +101 |
| CertificateVerify hash length #417 | +20 | +289 |
| NewSessionTicket #403 | +14 | +320 |
| empty application record #428 | +25 | +181 |
| Web PUT packet releases #395 | +11 | +377 |

Mostly already paid: our 92 test files are in their harness — 39
`nx_secure_test`, 18 `mdns_test`, 17 `dhcp_test`, 6 `netxduo_test`, 35 of them
new files in upstream's naming.

## 3. Three of ours are already upstream

| upstream | ours |
|---|---|
| #413 ASN.1 over-read | `7a858507`, file now byte-identical |
| #408 de-biased TCP ISN | `f829a5cb` |
| #409 mDNS big-endian | the `amiga-mdns-big-endian` merge |

Security parsing fix, core-TCP one-liner, portability. That is the profile.

## 4. Order

| # | tranche | commits | character |
|---|---|---|---|
| 1 | `x509` + `tls` parsing | ~18 | signature substitution, PKCS#1 v1.5 checked at the first byte, issuer-walk loop, SAN parsing, CBC IV reuse |
| 2 | `dns` | 7 | off-path cache poisoning |
| 3 | `mdns` | ~27 | races, refcounting, teardown ownership |
| 4 | `dhcpv6`+`dhcp` | ~29 | use-after-free on teardown, create-failure paths |
| 5 | `tcp` fixes | ~20 | option-list termination, RST-for-RST, RFC 1122 windows |
| 6 | features | ~35 | SACK, timestamps, SYN cache, TLP, ALPN, EtM, EMS |

247 non-merge commits. Tranches 1–2 touch no `nx_api.h`. Make the first
submission a sibling of one they already took.

## 5. Cherry-pick onto pristine upstream

| tranche 1+2 | clean | conflict |
|---|---|---|
| individually | 10 | 7 |
| cumulative, date order | 13 | 7 |

Every conflict is x509 and they conflict on each other, not on upstream: an
ordered series on our own earlier x509 rewrites. DNS is the cheapest start.

## 6. Compliance

Matching: file naming, `nxe_`/`nxde_` twins, SPDX header, `ptr -> field`
spacing (1,566 against 13), tabs (36 lines of ~17,000).

| gap | count |
|---|---|
| new files with the `FUNCTION / RELEASE` block | 17 of 25 |
| new files with the `DATE / NAME / DESCRIPTION` row | 0 of 25 |
| edits adding `resulting in version 6.x.x` | 0 |

**1,085 hunks across 144 upstream files**, each needing a history line that
cannot be generated: the version is whichever release accepts it.

## 7. Never upstream

Embedding hooks (witness a segment reaching its socket; a driver thread that
holds the IP lock processing input), `crypto68k` routing, and `WIP tcp:
timestamps on data segments, sequence accounting still wrong` — which needs
squashing regardless.

## 8. What a submission must satisfy

**ECA, and it is already on file.** `CONTRIBUTING.md` requires the Eclipse
Contributor Agreement and says it replaces the per-commit trailer: "Having an
ECA on file associated with the email address matching the 'Author' field of
your contribution's Git commits fulfills the DCO's requirement". Only 3 of the
last 40 commits on `dev` carry `Signed-off-by`. Proof it is in place:
`1ff784be`, `ccf51e1a` and `007e3cc1` are authored by `tinicuro@gmail.com`.
**Author every patch with that address** — the ECA bot blocks a PR whose
author email has no agreement.

**MISRA is not a contribution requirement.** Zero mentions across `common`,
`nx_secure`, `crypto_libraries` and `addons`; one incidental hit in an FTP
test; no job in `ci-dev.yml`; nothing in `CONTRIBUTING.md`. What a PR does
face is `ci-dev.yml`, which classifies the changed paths and runs the matching
regression suites (`dns_test`, `mdns_test`, `crypto_test`, `dhcp_test` …), so
our tests have to pass in their harness.

**AI-assisted contributions are routine, and the disclosure is a trailer.** 11
of the last 60 netxduo commits and 3 of 60 threadx commits name an AI
co-author — including five by `frederic.desbiens@eclipse-foundation.org`, who
is Eclipse Foundation staff, and two by `edouard.malot@gmail.com` using Claude:

    Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>
    Co-authored-by: Codex <codex@openai.com>
    Co-authored-by: Claude Fable 5 <noreply@anthropic.com>

No checkbox, no statement, no separate declaration. Our commits already carry
the same form. The Foundation's handbook still puts accuracy and IP vetting on
the contributor however the code was produced, and the exact wording it wants
is not stated anywhere we could find — the practice above is what merged.
