#!/usr/bin/env python3
"""Keep redistribution notices and complete source inseparable from the binaries.

SPDX-License-Identifier: MIT
"""

import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("pcmcia", ROOT / "tools/pcmcia-drivers.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class PackageTests(unittest.TestCase):
    def test_complete_package(self):
        PACKAGE.verify()

    def test_missing_or_changed_redistribution_files_fail_closed(self):
        original = PACKAGE.VENDOR
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="pcmcia-license-test-", dir=ROOT / "build") as directory:
            PACKAGE.VENDOR = Path(directory) / "PCMCIA"
            try:
                shutil.copytree(original, PACKAGE.VENDOR)
                for rel in ("Docs/GPL", "Docs/LGPL", "Docs/Hermes-LICENSE",
                            "Sources/prism2v2.lha", "Sources/xircomce2.lha", "Sources/pccard.lha"):
                    with self.subTest(file=rel):
                        path = PACKAGE.VENDOR / rel
                        data = path.read_bytes()
                        path.unlink()
                        with self.assertRaises(FileNotFoundError):
                            PACKAGE.verify()
                        path.write_bytes(data + b"changed")
                        with self.assertRaises(ValueError):
                            PACKAGE.verify()
                        path.write_bytes(data)
                PACKAGE.verify()
            finally:
                PACKAGE.VENDOR = original


class InstallerPackingTests(unittest.TestCase):
    def test_quotes_and_diagnostic_lines_survive_packing(self):
        sys.path.insert(0, str(ROOT / "install/tools"))
        try:
            from packscript import compact
            sample = '; comment\n  (set A "keep; this \\\"quote\\\"") ; drop\n(set B \'also; keep\')\n'
            self.assertEqual(compact(sample), '\n(set A "keep; this \\\"quote\\\"")\n(set B \'also; keep\')\n')
            source = (ROOT / "install/Install-AmiNetXDuo").read_text()
            packed = compact(source)
            self.assertLess(len(packed), len(source))
            self.assertEqual(packed.count("\n"), source.count("\n"))
        finally:
            sys.path.pop(0)


class InstalledMemoryTests(unittest.TestCase):
    def test_exec_version_and_allocations_do_not_change_installed_ram(self):
        # Compile the shipping helper against a small Exec model. V37/V38
        # ignore MEMF_TOTAL, so calling AvailMem there must fail this test.
        with tempfile.TemporaryDirectory(prefix="memory-probe-test-", dir=ROOT / "build") as directory:
            directory = Path(directory)
            for header in ("exec/execbase.h", "exec/memory.h", "proto/exec.h"):
                path = directory / header
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("")
            source = directory / "test.c"
            source.write_text(r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef uintptr_t ULONG;
struct Node { struct Node *ln_Succ; };
struct MemHeader { struct Node mh_Node; void *mh_Lower, *mh_Upper; ULONG mh_Free; };
struct ExecBase {
    struct { unsigned lib_Version; } LibNode;
    struct { struct Node *lh_Head; } MemList;
};
#define MEMF_TOTAL (1UL << 19)
struct ExecBase base, *SysBase = &base;
static unsigned depth, queries;
static void Forbid(void) { assert(depth == 0); ++depth; }
static void Permit(void) { assert(depth == 1); --depth; }
static ULONG AvailMem(ULONG flags) {
    assert(base.LibNode.lib_Version >= 39 && flags == MEMF_TOTAL && depth == 0);
    ++queries;
    return 12UL * 1024 * 1024;
}
#include "netprobe_memory.h"
int main(void) {
    static struct Node tail = { NULL };
    static struct MemHeader fast = { { &tail }, (void *)0x200000, (void *)0x980000, 0 };
    static struct MemHeader chip = { { &fast.mh_Node }, (void *)0x1000, (void *)0x80000, 0 };
    base.MemList.lh_Head = &chip.mh_Node;
    for (unsigned version = 37; version <= 40; ++version) {
        base.LibNode.lib_Version = version;
        queries = 0;
        ULONG expected = version < 39 ? 8UL * 1024 * 1024 - 4096 : 12UL * 1024 * 1024;
        assert(netprobe_memory_total() == expected);
        chip.mh_Free = 64;
        fast.mh_Free = 1024;
        assert(netprobe_memory_total() == expected);
        assert(depth == 0 && queries == (version < 39 ? 0U : 2U));
    }
    return 0;
}
''')
            program = directory / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(directory), "-I" + str(ROOT / "src/tools"),
                            str(source), "-o", str(program)], check=True)
            subprocess.run([str(program)], check=True)


class ResidentDeviceTests(unittest.TestCase):
    def test_rom_registered_and_absent_devices(self):
        with tempfile.TemporaryDirectory(prefix="resident-probe-test-", dir=ROOT / "build") as directory:
            directory = Path(directory)
            for header in ("exec/execbase.h", "exec/resident.h", "proto/exec.h"):
                path = directory / header
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("")
            source = directory / "test.c"
            source.write_text(r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
typedef int BOOL;
typedef const char *CONST_STRPTR;
#define FALSE 0
struct List { const char *name; };
struct ExecBase { struct List DeviceList; };
struct Resident { unsigned char rt_Type; };
#define NT_DEVICE 3
#define NT_LIBRARY 9
struct ExecBase base, *SysBase = &base;
static unsigned depth;
static const char *rom_name;
static struct Resident rom;
static void Forbid(void) { assert(depth == 0); ++depth; }
static void Permit(void) { assert(depth == 1); --depth; }
static void *FindName(struct List *list, CONST_STRPTR name) {
    assert(depth == 1 && list == &base.DeviceList);
    return list->name && strcmp(list->name, name) == 0 ? list : NULL;
}
static struct Resident *FindResident(CONST_STRPTR name) {
    assert(depth == 1);
    return rom_name && strcmp(rom_name, name) == 0 ? &rom : NULL;
}
#include "netprobe_device.h"
int main(void) {
    const char *names[] = { "uaenet.device", "amithlon1_net.device",
                           "powerne2k.device", "anothernet.device" };
    assert(!netprobe_device_present(NULL));
    assert(!netprobe_device_present(""));
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        rom_name = names[i];
        rom.rt_Type = NT_DEVICE;
        assert(netprobe_device_present(names[i]));
        assert(!netprobe_device_present("missing.device"));
        rom.rt_Type = NT_LIBRARY;
        assert(!netprobe_device_present(names[i]));
        rom_name = NULL;
        base.DeviceList.name = names[i];
        assert(netprobe_device_present(names[i]));
        assert(!netprobe_device_present("missing.device"));
        base.DeviceList.name = NULL;
        assert(!netprobe_device_present(names[i]));
        assert(depth == 0);
    }
    return 0;
}
''')
            program = directory / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(directory), "-I" + str(ROOT / "src/tools"),
                            str(source), "-o", str(program)], check=True)
            subprocess.run([str(program)], check=True)


if __name__ == "__main__":
    unittest.main()
