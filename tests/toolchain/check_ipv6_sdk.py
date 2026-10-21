#!/usr/bin/env python3
"""Compile the staged, public IPv6 ABI with each supported GCC runtime."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PROBE = r"""#include <exec/types.h>
@HEADERS@
#ifdef __cplusplus
#define CHECK(c) static_assert(c, #c)
#else
#define CHECK(c) _Static_assert(c, #c)
#endif
#define OFF(t, f) __builtin_offsetof(struct t, f)
#ifndef SIN6_LEN
#error Missing BSD IPv6 ABI
#endif
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
        count = 0
        for runtime in ("libnix", "newlib", "clib2"):
            for cpu in ("68000", "68020"):
                for lang, standard, compiler in (("c", "gnu11", "gcc"),
                                                  ("c++", "gnu++17", "g++")):
                    for order in (0, 1):
                        headers = ["sys/socket.h", "netinet/in.h", "aminetxduo/in6.h"]
                        if order:
                            headers.reverse()
                        source = work / "probe.c"
                        source.write_text(PROBE.replace("@HEADERS@", "\n".join(
                            f"#include <{h}>" for h in headers)))
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
        print(f"IPv6 SDK: {count} ABI/include-order compile checks passed")


if __name__ == "__main__":
    main()
