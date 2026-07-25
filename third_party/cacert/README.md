# The trust store this project ships

`cacert.pem` is a **verbatim, dated snapshot** of the Mozilla CA root
certificate set, as extracted and published by the curl project.

| | |
|---|---|
| Source | <https://curl.se/ca/cacert.pem> |
| Snapshot | "Certificate data from Mozilla as of: Thu Jul 16 03:12:01 2026 GMT" |
| SHA-256 | `3ff344e30b9b1ed2971044eabb438a08f2e2245ddb5f8ab1a3ad8b63ab4eaf91` |
| Roots | 119 |
| Licence | **MPL 2.0** — see below.  The rest of this project is MIT. |

`cacert.pem.sha256` is curl.se's own published checksum file, copied verbatim,
so the snapshot can be checked against upstream without trusting this
repository:

    cd third_party/cacert && shasum -a 256 -c cacert.pem.sha256

## Why it is vendored, and not fetched

The build turns this file into `DEVS:Internet/certificates` with
`tools/mkcertstore.py`.  Before this file existed, CMake looked for a CA
bundle on the *host* — `/etc/ssl/cert.pem` and friends — which meant a release
carried whatever roots the release engineer's laptop happened to have, and a
build on a machine with no bundle shipped a `tls.library` that refused every
connection with `TLS_ERR_TRUSTSTORE`.

The three candidates were:

1. **Fetch a pinned URL at release time.**  Rejected: it makes the build
   depend on a third party's web server being up, which is the opposite of
   reproducible, and it is unbuildable offline or in ten years' time.
2. **Vendor the generated `certificates` file.**  Rejected: it is 126 KB of
   opaque binary in git that nobody can review, and it hides the input the
   licence attaches to.
3. **Vendor the PEM snapshot and pin both hashes.**  Taken.  The input is
   reviewable text, the licence obligation attaches to a file that carries its
   own provenance, and the build is offline and byte-reproducible.

Two hashes are pinned, and both are checked at configure/build time:

* `cacert.pem.sha256` — the input, so a corrupted or swapped bundle fails.
* `certificates.sha256` — the *generated store*, so the build fails if
  anything at all differs: a different bundle, a different `mkcertstore.py`, a
  host CA file leaking in.  That second pin is what makes "the same source tree
  produces a byte-identical store on any host" a checked claim rather than an
  intention.

A build that points `-DAMINETXDUO_CA_BUNDLE=` at some other PEM is still
supported; it skips the output pin, says so, and only enforces a floor on the
root count.

## Updating it

Roots change on a scale of months.  When you want a newer set:

    curl -o third_party/cacert/cacert.pem        https://curl.se/ca/cacert.pem
    curl -o third_party/cacert/cacert.pem.sha256 https://curl.se/ca/cacert.pem.sha256
    (cd third_party/cacert && shasum -a 256 -c cacert.pem.sha256)
    python3 tools/mkcertstore.py -o /tmp/certificates third_party/cacert/cacert.pem
    shasum -a 256 /tmp/certificates | sed 's| .*|  certificates|' \
        > third_party/cacert/certificates.sha256

Then update the table at the top of this file and commit all three together.
An end user does not need any of this: `tools/mkcertstore.py` turns any PEM
bundle into the file, and `tls.library` re-reads the store on every
connection, so replacing `DEVS:Internet/certificates` on the Amiga is the whole
update story.

## The licence, considered

`cacert.pem` is a compilation of Mozilla's `certdata.txt`, and that file is
covered by the **Mozilla Public License 2.0**.  The rest of AmiNetXDuo is MIT.
That combination is fine, and the reasoning matters enough to write down:

* **MPL 2.0 is file-scoped copyleft.**  §1.10 defines a Modification as a
  change to the contents of a Covered File.  §3.3 then says a Larger Work may
  be distributed under other terms provided the MPL-covered files stay under
  the MPL and keep their notices.  MIT source next to an MPL data file is
  exactly the case §3.3 is written for.  It is what `certifi` (MPL 2.0) inside
  MIT and BSD Python applications, and `webpki-roots` (MPL 2.0) inside Rust
  applications, have done for over a decade.
* **We do not modify it.**  The snapshot is byte-identical to upstream and its
  hash is recorded, so there is nothing to relicense.
* **The generated store is treated as covered too.**  `DEVS:Internet/certificates`
  is a re-encoding of the same certificate set, so the conservative reading is
  that it is a Modification of a Covered File.  We therefore ship it as
  MPL-2.0-covered and say so, rather than argue that a change of container
  launders the licence.  §3.2 asks that recipients of an executable form be
  told how to get the Source Code Form: `dist/ReadMe` names this file, its
  upstream URL and `tools/mkcertstore.py`, and the archive carries that text.
* **No copyleft reaches our code.**  MPL 2.0 §3.3's "Larger Work" permission is
  unconditional as to the other files, and nothing in this project is a
  derivative of the certificate data.  `bsdsocket.library`, `tls.library` and
  the commands remain MIT.
* **The alternative was considered.**  There is no widely-audited root bundle
  under a permissive licence: certifi is MPL 2.0, webpki-roots is MPL 2.0,
  Apple's and Microsoft's stores are not redistributable, and hand-picking a
  subset would be arbitrary and less trustworthy.  Shipping no roots at all is
  the status quo this replaces, and it is what made TLS useless by default.

SPDX-License-Identifier: MPL-2.0
