#!/usr/bin/env python3
"""Prepare Dropbear's scp.c for the AmigaOS process bridge.

Calls to a function defined in the same Amiga HUNK object are assembled as
PC-relative branches, so making upstream do_cmd() weak cannot interpose them.
Rename that one definition in a build-directory copy; calls remain do_cmd()
and resolve to amiga_scp.c.  Also close ssh's stdin after a download: upstream
discards that descriptor while its Unix ssh happens to exit, but the separate
Amiga Process correctly waits for EOF.  A normal local invocation returns
through the AmigaDOS command boundary instead of calling process-level exit().
Refuse any source shape other than the pinned one.
"""

from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: prepare-scp.py INPUT OUTPUT", file=sys.stderr)
        return 2

    source = Path(sys.argv[1]).read_text()
    needle = "\nint\ndo_cmd(char *host, char *remuser, char *cmd, int *fdin, int *fdout)\n"
    replacement = (
        "\nint do_cmd(char *host, char *remuser, char *cmd, "
        "int *fdin, int *fdout);\n"
        "\nint\n"
        "dropbear_unix_do_cmd(char *host, char *remuser, char *cmd, "
        "int *fdin, int *fdout)\n"
    )
    count = source.count(needle)
    if count != 1:
        print(
            f"prepare-scp: do_cmd definition matched {count} times, expected 1",
            file=sys.stderr,
        )
        return 1

    source = source.replace(needle, replacement)

    close_needle = (
        "\t\tsink(1, argv + argc - 1, src);\n"
        "\t\t(void) close(remin);\n"
        "\t\tremin = remout = -1;\n"
    )
    close_replacement = (
        "\t\tsink(1, argv + argc - 1, src);\n"
        "\t\t(void) close(remin);\n"
        "\t\t(void) close(remout);\n"
        "\t\tremin = remout = -1;\n"
    )
    count = source.count(close_needle)
    if count != 1:
        print(
            f"prepare-scp: download close matched {count} times, expected 1",
            file=sys.stderr,
        )
        return 1
    source = source.replace(close_needle, close_replacement)

    # Unix scp skips waitpid() when a local transfer has already set errs.
    # Our child owns a loaded SegList, signal and two shared-memory endpoints;
    # leaving those to atexit makes the next AmigaDOS command race incomplete
    # teardown.  Preserve the first error, but always take the normal reap path
    # whenever do_cmd() started a child.
    reap_needle = "\tif (do_cmd_pid != -1 && errs == 0) {\n"
    reap_replacement = "\tif (do_cmd_pid != -1) {\n"
    count = source.count(reap_needle)
    if count != 1:
        print(
            f"prepare-scp: final reap matched {count} times, expected 1",
            file=sys.stderr,
        )
        return 1
    source = source.replace(reap_needle, reap_replacement)

    # A normal local SCP invocation should return through RunCommand/System's
    # command boundary.  Upstream terminates the whole Unix process here;
    # doing that after our atexit child cleanup bypasses the Amiga Shell's
    # ordinary command-return path and leaves the next command unable to
    # start.  The two remote-server exit() calls remain unchanged.
    return_needle = (
        "\t}\n"
        "\texit(errs != 0);\n"
        "}\n"
        "#endif /* DBMULTI_scp stuff */\n"
    )
    return_replacement = (
        "\t}\n"
        "\treturn errs != 0;\n"
        "}\n"
        "#endif /* DBMULTI_scp stuff */\n"
    )
    count = source.count(return_needle)
    if count != 1:
        print(
            f"prepare-scp: final return matched {count} times, expected 1",
            file=sys.stderr,
        )
        return 1
    source = source.replace(return_needle, return_replacement)

    Path(sys.argv[2]).write_text(source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
