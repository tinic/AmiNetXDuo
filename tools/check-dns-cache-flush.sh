#!/usr/bin/env bash
#
# A NAME SERVER TAKEN AWAY TAKES ITS ANSWERS WITH IT.
#
# nxd_dns.c answers a lookup from its cache BEFORE it looks for a server
# (_nx_dns_host_resource_data_by_name_get: _nx_dns_cache_find_answer first,
# NX_DNS_NO_SERVER after).  So a name resolved before NetShutdown went on
# resolving for the rest of its TTL with no interface on the machine, and a
# program that decides "online" by gethostbyname() -- AreWeOnline, the tool
# behind AmiTCP_NG issue #4 -- read a shut-down machine as connected.  Roadshow
# refuses the lookup; AmiTCP_NG 4.1.6 fixed the same thing ("DNS answers are
# discarded when the name servers change").
#
# The fix is in the vendored NetX Duo, third_party/netxduo/addons/dns/nxd_dns.c:
# _nx_dns_cache_drop() runs in every path that removes a server from the
# client, so no caller in src/ has to remember.  This proves, on every commit
# and without an emulator, that the submodule the tree pins still carries it:
# a submodule bump to an upstream that lacks the patch would otherwise bring
# the defect back silently, and the DNS client is not in the host tier for a
# unit test to notice.  Same shape as tools/check-installer-transaction.sh --
# an order in a text.
#
# SPDX-License-Identifier: MIT
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FILE="$ROOT/third_party/netxduo/addons/dns/nxd_dns.c"
[ -f "$FILE" ] || { echo "dns_cache_flush=FAIL reason=no_file"; exit 2; }

python3 - "$FILE" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path, errors='ignore').read()

# The helper must exist and do the four things _nx_dns_cache_initialize() does.
m = re.search(r'static VOID\s+_nx_dns_cache_drop\s*\(NX_DNS \*dns_ptr\)\s*\{(.*?)\n\}', text, re.S)
if not m:
    print("dns_cache_flush=FAIL reason=no_helper")
    print("  _nx_dns_cache_drop() is not defined in", path)
    sys.exit(1)
body = m.group(1)
for need in ('memset(dns_ptr -> nx_dns_cache', 'nx_dns_rr_count = 0',
             'nx_dns_string_count = 0', 'nx_dns_string_bytes = 0'):
    if need not in body:
        print("dns_cache_flush=FAIL reason=helper_incomplete missing=%s" % need)
        sys.exit(1)

# Every server-removal function must call it before it releases the mutex.
lines = text.split('\n')
fns = ('_nx_dns_server_remove_internal', '_nx_dns_server_remove_all')
bad = []
for fn in fns:
    start = None
    for i, l in enumerate(lines):
        # The definition, not the prototype at the top of the file: a
        # prototype ends in a semicolon, a definition opens a brace next.
        if re.match(r'^(static\s+)?UINT\s+' + re.escape(fn) + r'\s*\(', l) \
                and not l.rstrip().endswith(';'):
            start = i
            break
    if start is None:
        bad.append(fn + ':missing')
        continue
    j = start + 1
    seen = False
    while j < len(lines) and not lines[j].startswith('}'):
        if '_nx_dns_cache_drop(dns_ptr)' in lines[j]:
            seen = True
            break
        j += 1
    if not seen:
        bad.append(fn + ':no_drop')

if bad:
    print("dns_cache_flush=FAIL unflushed=%s" % ",".join(bad))
    print("  a server is removed from the NetX DNS client here and the cache")
    print("  it filled is kept; every answer in it came from a server that is gone")
    sys.exit(1)

print("dns_cache_flush=PASS functions=%d" % len(fns))
PY
