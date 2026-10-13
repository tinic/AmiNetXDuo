#!/usr/bin/env python3
"""CLI coverage for the Python report tool shipped in Developer/Profile."""
import ast
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'tools/profiler/profreport.py'
VALUES = {'--exe': 'missing.exe', '--map': 'empty.map', '--objdir': '.',
          '--nm': 'missing-nm', '--ndk': '.', '--phase': 'missing', '--top': '1',
          '--symbol': 'missing', '--max-age': '0', '--allow-stale': None,
          '--allow-unresolved': None, '--by-module': None, '--folded': 'out.folded',
          '--trace': 'out.json', '--contain': 'missing.ranges',
          '--lib': 'exe=missing.library,name=missing.library'}


class HostReportTests(unittest.TestCase):
    def test_inventory(self):
        names = {n.args[0].value for n in ast.walk(ast.parse(SOURCE.read_text()))
                 if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
                 and n.func.attr == 'add_argument' and n.args
                 and isinstance(n.args[0], ast.Constant)}
        self.assertEqual(names, {'profile'} | set(VALUES))
        self.assertIn('"$ROOT/tools/profiler/profreport.py"', (ROOT/'dist/make-dist.sh').read_text())

    def test_cli_options_and_invalid_files(self):
        spec = importlib.util.spec_from_file_location('report', SOURCE)
        report = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(report)
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
            base = Path(tmp)
            f = [0] * 28
            f[0], f[1], f[2], f[3] = report.PROF_MAGIC, 3, 1, 100
            f[4] = f[5] = 1
            f[9], f[14], f[18], f[19], f[21] = 4, 40, 227 * 312, 3546895, 4
            blob = report.HDR.pack(*f) + report.SAMPLE.pack(0x1000, 0, 0, 0, 0)
            (base/'valid.prof').write_bytes(blob)
            (base/'empty.prof').write_bytes(b'')
            (base/'short.prof').write_bytes(blob[:-1])
            (base/'extra.prof').write_bytes(blob+b'x')
            (base/'empty.map').write_text('')
            cases = [[], ['--help'], ['--unknown'], ['missing.prof'],
                     ['empty.prof'], ['short.prof'], ['extra.prof'], ['valid.prof']]
            for flag, val in VALUES.items():
                option = [flag] + ([val] if val is not None else [])
                cases.append(['valid.prof', *option])
                cases.append(['valid.prof', *option, *option])
                if val is not None:
                    cases.append(['valid.prof', flag])
                    for bad in ('', 'not-a-value', 'x'*2048):
                        cases.append(['valid.prof', flag, bad])
            for flag in ('--top', '--max-age'):
                for val in ('-1', '0', '2147483647', '2147483648', '-2147483649', 'nan', 'inf'):
                    cases.append(['valid.prof', flag, val])
            for args in cases:
                with self.subTest(args=args):
                    p = subprocess.run([sys.executable, '-B', str(SOURCE), *args],
                                       cwd=tmp, text=True, capture_output=True, timeout=10)
                    self.assertIn(p.returncode, (0, 1, 2), p.stderr)
                    self.assertNotIn('Traceback (most recent call last)', p.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
