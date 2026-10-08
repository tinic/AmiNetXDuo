#!/usr/bin/env python3
"""Relink the actual full bsdsocket library with only the research TX backend.

Diagnostic integration gate, NOT a deployable library. Both modes match the
full build's stripping policy for an equal-feature file-size comparison;
--lto requires actual LTO compile/link flags. Neither proves runtime/performance.
Uses the full build's flags/vendor archives; removes the scheduler/Exec port
archives and the baton object. Missing contracts fail the link; no success
stubs, ignored unresolved symbols, protocol fixtures or PRNG replacements.
SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BACKEND = ["wait.c", "clock_schedule.c", "tx_bridge.c", "netx_resume.c",
           "tx_bridge_exec.c", "exec_wait.c", "exec_thread.c", "exec_clock.c", "exec_caller.c", "exec_kernel.c", "exec_netstack.c"]
# Keep upstream service bodies rather than reproducing their wait/list logic.
SERVICES = ["thread_timeout", "thread_sleep", "thread_wait_abort", "thread_info_get", "thread_suspend",
            "mutex_cleanup", "mutex_delete", "event_flags_get", "event_flags_set",
            "event_flags_cleanup", "event_flags_delete", "timer_create",
            "timer_activate", "timer_change", "timer_delete",
            "semaphore_create", "semaphore_delete", "semaphore_get", "semaphore_put",
            "semaphore_cleanup"]
RENAMED = {"thread_wait_abort", "thread_suspend", "mutex_delete", "event_flags_get", "event_flags_set",
           "event_flags_delete", "timer_create", "timer_activate", "timer_change",
           "timer_delete", "semaphore_create", "semaphore_delete", "semaphore_get", "semaphore_put"}
FORBIDDEN_ARCHIVES = {"libthreadx.a", "libthreadx_port.a"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, cwd, log):
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    with log.open("a") as out:
        out.write(shlex.join([str(c) for c in command]) + "\n" + result.stdout)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--full-build", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--report", type=Path, required=True)
    ap.add_argument("--lto", action="store_true",
                    help="require a full LTO build and match its symbol stripping policy")
    args = ap.parse_args()
    full = args.full_build.resolve()
    work = args.work.resolve()
    # Work belongs in this repository's disposable build area. Never copy or
    # rewrite a tracked shipping archive/source or another owner's build.
    if not work.is_relative_to(ROOT / "build") or work == full:
        ap.error("--work must be a distinct disposable directory below repository build/")
    work.mkdir(parents=True, exist_ok=False)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    log = args.report.with_suffix(".log")
    log.write_text("")
    entries = json.loads((full / "compile_commands.json").read_text())
    exemplar = next(e for e in entries if Path(e["file"]) == ROOT / "src/netstack/netstack.c")
    words = shlex.split(exemplar["command"])
    compiler = Path(words[0])
    flags = []
    i = 1
    while i < len(words):
        if words[i] in ("-o", "-c"):
            i += 2
        else:
            flags.append(words[i])
            i += 1
    if any(f.startswith("-flto") for f in flags) != args.lto:
        ap.error("--lto must match the full build's actual compile flags")
    # Retain every shipping feature/layout definition and calling convention.
    # Function sections only let the real full link choose its actual closure.
    flags += ["-I" + str(HERE), "-Werror", "-ffunction-sections"]
    sources = [(HERE / name, None) for name in BACKEND]
    sources += [(ROOT / "third_party/threadx/common/src" / ("tx_" + name + ".c"), name)
                for name in SERVICES]
    objects = []
    for source, service in sources:
        obj = work / (source.name + ".o")
        command = [str(compiler), *flags]
        if service in RENAMED:
            original_name = "wait_abort" if service == "thread_wait_abort" else service
            command += [f"-D_tx_{service}=anx_tx_original_{original_name}"]
        command += ["-c", str(source), "-o", str(obj)]
        result = run(command, Path(exemplar["directory"]), log)
        if result.returncode:
            raise SystemExit("full-profile research compile failed; see " + str(log))
        objects.append(obj)
    # The archive index must see slim LTO IR symbols through the GCC plugin.
    ar = compiler.with_name("m68k-amigaos-gcc-ar")
    backend = work / "libresearch_library_backend.a"
    result = run([str(ar), "rcs", str(backend), *map(str, objects)], work, log)
    if result.returncode:
        raise SystemExit("backend archive failed")
    original = full / "src/netstack/libaminetxduo_netstack.a"
    netstack = work / original.name
    shutil.copyfile(original, netstack)
    members = run([str(ar), "t", str(netstack)], work, log).stdout.splitlines()
    if members.count("netstack_baton.c.obj") != 1:
        raise SystemExit("expected exactly one original baton object")
    result = run([str(ar), "d", str(netstack), "netstack_baton.c.obj"], work, log)
    if result.returncode or "netstack_baton.c.obj" in run([str(ar), "t", str(netstack)], work, log).stdout:
        raise SystemExit("baton removal failed")
    # Actual parent integration with the one-shot reservation before the
    # unchanged upstream IP constructor. All shipping feature flags stay on.
    if members.count("netstack.c.obj") != 1:
        raise SystemExit("expected exactly one parent netstack object")
    parent_object = work / "netstack.c.obj"
    result = run([str(compiler), *flags, "-DAMINETXDUO_EXEC_RESEARCH",
                  "-c", str(ROOT / "src/netstack/netstack.c"), "-o", str(parent_object)],
                 Path(exemplar["directory"]), log)
    if result.returncode:
        raise SystemExit("research parent reservation compile failed")
    if run([str(ar), "d", str(netstack), "netstack.c.obj"], work, log).returncode or \
       run([str(ar), "r", str(netstack), str(parent_object)], work, log).returncode:
        raise SystemExit("parent reservation object replacement failed")
    link_dir = full / "src/bsdsocket"
    original_link = shlex.split((link_dir / "CMakeFiles/bsdsocket_library.dir/link.txt").read_text())
    if args.lto and not any(f.startswith("-flto") for f in original_link):
        raise SystemExit("LTO compile flags require an actual LTO link")
    # The real library vectors must release only Exec's Open/Close-owned outer
    # exclusion around blocking work, preserving any additional caller nesting.
    library_entry = next(e for e in entries if Path(e["file"]) == ROOT / "src/bsdsocket/library.c")
    library_words = shlex.split(library_entry["command"])
    library_flags = []
    i = 1
    while i < len(library_words):
        if library_words[i] in ("-o", "-c"):
            i += 2
        else:
            library_flags.append(library_words[i])
            i += 1
    library_object = work / "library.c.obj"
    result = run([library_words[0], *library_flags, "-Werror", "-DAMINETXDUO_EXEC_RESEARCH",
                  "-c", str(ROOT / "src/bsdsocket/library.c"), "-o", str(library_object)],
                 Path(library_entry["directory"]), log)
    if result.returncode:
        raise SystemExit("research library lifecycle compile failed")
    if sum(Path(word).name == "library.c.obj" for word in original_link) != 1:
        raise SystemExit("expected exactly one original library lifecycle object")
    output = work / "bsdsocket-research-unverified.library"
    map_file = output.with_suffix(".map")
    command = [original_link[0]]
    removed = []
    for word in original_link[1:]:
        if word.startswith("-Wl,-Map="):
            command += ["-Wl,-Map=" + str(map_file)]
        elif word == "bsdsocket.library":
            command += [str(output)]
        elif word.endswith(".a") and Path(word).name in FORBIDDEN_ARCHIVES:
            removed.append(word)
        elif word.endswith(".a") and Path(word).name == original.name:
            command += [str(netstack)]
        elif Path(word).name == "library.c.obj":
            command += [str(library_object)]
        else:
            command.append(word)
    if {Path(w).name for w in removed} != FORBIDDEN_ARCHIVES:
        raise SystemExit("both old ThreadX archives must be removed")
    # Repeat full dependency group resolution without bringing back the original
    # scheduler. Every explicit object (romtag/LVO roots included) is unchanged.
    first_archive = next(i for i, word in enumerate(command) if word.endswith(".a"))
    command.insert(first_archive, "-Wl,--start-group")
    command += [str(backend), "-Wl,--end-group"]
    if any(Path(w).name in FORBIDDEN_ARCHIVES for w in command):
        raise SystemExit("original scheduler fallback still present")
    result = run(command, link_dir, log)
    unresolved = sorted(set(re.findall(r"undefined reference to [`']([^'`\n]+)['`]", result.stdout)))
    # A failed linker may leave a partial image. Never leave it available as a
    # runnable test/candidate. A successful link is still explicitly unverified.
    if result.returncode:
        output.unlink(missing_ok=True)
    unstripped = {"bytes": output.stat().st_size, "sha256": digest(output)} if output.exists() else None
    cache = (full / "CMakeCache.txt").read_text()
    keep = re.search(r"^AMINETXDUO_KEEP_SYMBOLS:BOOL=(ON|OFF)$", cache, re.M)
    if not keep:
        raise SystemExit("cannot establish baseline symbol policy")
    symbol_policy = "retain symbols" if keep[1] == "ON" else "strip symbols"
    if output.exists() and keep[1] == "OFF":
        strip = re.search(r"^CMAKE_STRIP:FILEPATH=(.+)$", cache, re.M)
        if not strip or run([strip[1], str(output)], work, log).returncode:
            output.unlink(missing_ok=True)
            raise SystemExit("matched baseline stripping failed")
    def git(*args):
        return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()
    baseline = link_dir / "bsdsocket.library"
    report = {
        "source_commit": git("rev-parse", "HEAD"),
        "source_dirty": bool(git("status", "--porcelain")),
        "netx_pin": git("-C", "third_party/netxduo", "rev-parse", "HEAD"),
        "threadx_pin": git("-C", "third_party/threadx", "rev-parse", "HEAD"),
        "full_profile_flags": flags,
        "method": "Actual full library objects/vendor archives, full-profile compiled research backend and retained pinned TX bodies; old ThreadX scheduler/port archives removed, netstack baton object removed. No fixtures/stubs/unresolved-symbol bypass. Matched full-feature " + ("LTO" if args.lto else "non-LTO") + " compile/link and symbol policy; file size only, runtime/performance unverified.",
        "lto": args.lto,
        "research_symbol_policy": symbol_policy,
        "research_unstripped_binary": unstripped,
        "research_binary": {"bytes": output.stat().st_size, "sha256": digest(output)} if output.exists() else None,
        "baseline_binary": {"bytes": baseline.stat().st_size, "sha256": digest(baseline)},
        "removed_scheduler_archives": removed,
        "removed_netstack_members": ["netstack_baton.c.obj"],
        "research_parent_objects": ["netstack.c.obj: actual production source/full flags + AMINETXDUO_EXEC_RESEARCH IP reservation and quiescent loopback lifecycle",
                                    "library.c.obj: actual production source/own full flags + AMINETXDUO_EXEC_RESEARCH Close-owned outer Forbid release"],
        "library_full_profile_flags": library_flags,
        "backend_sources": [str(p.relative_to(ROOT)) for p, _ in sources],
        "link_returncode": result.returncode,
        "unresolved_symbols": unresolved,
        "link_status": "PASS_LINK_ONLY_UNVERIFIED" if result.returncode == 0 else "FAIL_OPEN_CONTRACTS",
        "runtime_status": "NOT_RUN; no full-library lifecycle or public socket execution proved",
        "log": str(log),
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{report['link_status']}: {len(unresolved)} unresolved symbols; report {args.report}")
    return 0 if result.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
