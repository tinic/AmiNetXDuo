#!/usr/bin/env python3
"""Compile the staged, public IPv6 ABI with each supported GCC runtime."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PROBE = r"""#include <exec/types.h>
@HEADERS@
#define CAT_(a,b) a##b
#define CAT(a,b) CAT_(a,b)
#define CHECK(c) typedef char CAT(check_,__LINE__)[(c) ? 1 : -1]
#define OFF(t, f) __builtin_offsetof(struct t, f)
#ifndef SIN6_LEN
#error Missing BSD IPv6 ABI
#endif
CHECK(AF_INET6 == 28 && AF_INET6 != AF_IPX && AF_MAX > AF_INET6);
CHECK(PF_INET6 == AF_INET6);
#if defined(IP_PKTINFO) || defined(AI_ADDRCONFIG)
#error Private extensions leaked into the common SDK
#endif
CHECK(IPV6_V6ONLY == 27 && IPV6_CHECKSUM == 26);
CHECK(IPV6_TCLASS == 36 && IPV6_RECVPKTINFO == 61);
CHECK(AI_MASK == (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV));
CHECK(AI_NUMERICSERV == 16 && EAI_NONAME == -2);
CHECK(sizeof(struct in6_pktinfo) == 20);
CHECK(sizeof(struct if_nameindex) == 8 && IF_NAMESIZE == 16);
CHECK(sizeof(struct sockaddr_dl) <= sizeof(struct sockaddr_storage));
CHECK(__alignof__(struct sockaddr_storage) >= __alignof__(struct sockaddr_dl));
CHECK(CMSG_SPACE(1) == 16 && CMSG_LEN(1) == 13);
CHECK(sizeof(struct sockaddr_in) == 16);
CHECK(OFF(sockaddr_in, sin_len) == 0);
CHECK(OFF(sockaddr_in, sin_family) == 1);
CHECK(sizeof(struct sockaddr_in6) == 28);
CHECK(OFF(sockaddr_in6, sin6_len) == 0);
CHECK(OFF(sockaddr_in6, sin6_family) == OFF(sockaddr, sa_family));
CHECK(OFF(sockaddr_in6, sin6_port) == 2);
CHECK(OFF(sockaddr_in6, sin6_flowinfo) == 4);
CHECK(OFF(sockaddr_in6, sin6_addr) == 8);
CHECK(OFF(sockaddr_in6, sin6_scope_id) == 24);
CHECK(sizeof(struct sockaddr_storage) == 128);
CHECK(OFF(sockaddr_storage, ss_family) == OFF(sockaddr, sa_family));
CHECK(__alignof__(struct sockaddr_storage) >= __alignof__(struct sockaddr_in));
CHECK(__alignof__(struct sockaddr_storage) >= __alignof__(struct sockaddr_in6));
struct sockaddr_in6 probe = {28, AF_INET6, 0x1234, 0, IN6ADDR_LOOPBACK_INIT, 7};
"""


PRIVATE_PROBE = r"""
#include <aminetxduo/addrinfo.h>
#include <aminetxduo/cmsg.h>
#include <aminetxduo/icmp6.h>
CHECK(AI_ADDRCONFIG == 0x0400 && !(AI_MASK & AI_ADDRCONFIG));
CHECK(IP_PKTINFO == 26 && IP_PKTINFO != IP_RETOPTS);
CHECK(sizeof(struct in_pktinfo) == 12);
CHECK(OFF(in_pktinfo, ipi_ifindex) == 0);
CHECK(OFF(in_pktinfo, ipi_spec_dst) == 4);
CHECK(OFF(in_pktinfo, ipi_addr) == 8);
CHECK(sizeof(struct icmp6_filter) == 32);
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", required=True, type=Path)
    args = parser.parse_args()
    toolchain = args.toolchain.resolve()
    with tempfile.TemporaryDirectory(prefix="anx-ipv6-sdk-") as work:
        work = Path(work)
        sdk = work / "Developer"
        subprocess.run(["bash", str(ROOT / "tools/stage-developer.sh"), str(sdk)],
                       check=True)
        # Retire our exact historical filter-only header, while preserving
        # unrelated complete protocol headers in a reused stage.
        obsolete = sdk / "include/netinet/icmp6.h"
        private = sdk / "include/aminetxduo/icmp6.h"
        fixture = ROOT / "tests/toolchain/fixtures/icmp6-filter-only.h"
        obsolete.write_bytes(fixture.read_bytes())
        subprocess.run(["bash", str(ROOT / "tools/stage-developer.sh"), str(sdk)], check=True)
        if obsolete.exists() or not private.exists():
            raise SystemExit("obsolete filter-only header survived restaging")
        foreign = "/* Unrelated complete user ICMPv6 header. */\n"
        obsolete.write_text(foreign)
        subprocess.run(["bash", str(ROOT / "tools/stage-developer.sh"), str(sdk)], check=True)
        if obsolete.read_text() != foreign:
            raise SystemExit("restaging overwrote an unrelated ICMPv6 header")
        obsolete.unlink()
        count = 0
        for runtime in ("libnix", "newlib", "clib2"):
            for cpu in ("68000", "68020"):
                for lang, standard, compiler in (("c", "gnu89", "gcc"),
                                                  ("c", "gnu11", "gcc"),
                                                  ("c++", "gnu++98", "g++"),
                                                  ("c++", "gnu++17", "g++")):
                    for order in (0, 1):
                        headers = ["sys/socket.h", "netinet/in.h", "netdb.h",
                                   "net/if.h", "net/if_dl.h"]
                        if order:
                            headers.reverse()
                        source = work / "probe.c"
                        source.write_text(PROBE.replace("@HEADERS@", "\n".join(
                            f"#include <{h}>" for h in headers)) + PRIVATE_PROBE)
                        command = [str(toolchain / "bin" / f"m68k-amigaos-{compiler}"),
                                   "-x", lang, f"-std={standard}", f"-m{cpu}",
                                   "-Os", "-Wall", "-Wextra", "-Werror",
                                   "-I" + str(sdk / "include"),
                                   "-I" + str(toolchain / "m68k-amigaos/ndk-include")]
                        if runtime != "libnix":
                            command.append("-mcrt=" + runtime)
                        result = subprocess.run(command + ["-c", str(source), "-o",
                                                            str(work / "probe.o")],
                                                text=True, capture_output=True)
                        if result.returncode:
                            raise SystemExit(f"{runtime}/{cpu}/{lang}/order={order}:\n"
                                             + result.stdout + result.stderr)
                        count += 1
        # Link the standard constant objects as C and C++ with each runtime.
        # This SDK fallback is optional when a runtime already owns the data.
        for runtime in ("libnix", "newlib", "clib2"):
            for lang, compiler in (("c", "gcc"), ("c++", "g++")):
                source = work / "constants.c"
                source.write_text("#include <netinet/in.h>\n"
                                  "int main(void) { return in6addr_any.s6_addr[0] + "
                                  "in6addr_loopback.s6_addr[15] - 1; }\n")
                command = [str(toolchain / "bin" / f"m68k-amigaos-{compiler}"),
                           "-m68000", "-Os", "-I" + str(sdk / "include"),
                           "-I" + str(toolchain / "m68k-amigaos/ndk-include")]
                if runtime != "libnix":
                    command.append("-mcrt=" + runtime)
                subprocess.run(command + ["-x", lang, str(source),
                    str(sdk / "support/in6_addr.c"), "-o", str(work / "constants")],
                    check=True, capture_output=True)
        # The if_* prototypes follow the stub convention of the rest of the
        # netinclude: __stdargs, so -mregparm cannot move their arguments
        # into registers, and absent under __NO_NET_API, so a program can
        # declare its own.
        probes = {
            "callconv": "#include <exec/types.h>\n#include <net/if.h>\n"
                        "typedef __ULONG (__stdargs *index_fn)(const char *);\n"
                        "index_fn probe = &if_nametoindex;\n",
            "no_net_api": "#define __NO_NET_API\n#include <exec/types.h>\n"
                          "#include <net/if.h>\n"
                          "LONG if_nametoindex(STRPTR name);\n"
                          "LONG if_freenameindex(APTR list);\n",
        }
        for name, text in probes.items():
            for lang, compiler in (("c", "gcc"), ("c++", "g++")):
                source = work / f"{name}.c"
                source.write_text(text)
                result = subprocess.run([
                    str(toolchain / "bin" / f"m68k-amigaos-{compiler}"),
                    "-x", lang, "-m68000", "-mregparm=2", "-Os",
                    "-Wall", "-Werror", "-I" + str(sdk / "include"),
                    "-I" + str(toolchain / "m68k-amigaos/ndk-include"),
                    "-c", str(source), "-o", str(work / f"{name}.o")],
                    text=True, capture_output=True)
                if result.returncode:
                    raise SystemExit(f"net/if.h {name}/{lang}:\n"
                                     + result.stdout + result.stderr)
        # Including an addendum after the installed, old headers must not
        # quietly produce an application with the previous IPv6 layout.
        old_header = toolchain / "m68k-amigaos/ndk-include/netinet/in.h"
        if "#define SIN6_LEN" not in old_header.read_text(encoding="latin1"):
            source = work / "old.c"
            source.write_text('#include <aminetxduo/in6.h>\n')
            result = subprocess.run([
                str(toolchain / "bin/m68k-amigaos-gcc"),
                "-I" + str(ROOT / "include"),
                "-I" + str(toolchain / "m68k-amigaos/ndk-include"),
                "-c", str(source), "-o", str(work / "old.o")],
                text=True, capture_output=True)
            if result.returncode == 0 or "Use the AmiNetXDuo Developer/include" not in result.stderr:
                raise SystemExit("old IPv6 SDK did not fail with the migration diagnostic")
        print(f"IPv6 SDK: {count} ABI/include-order compile checks, 6 constant-symbol "
              "links, 4 net/if.h convention probes and stale-stage migration passed")


if __name__ == "__main__":
    main()
