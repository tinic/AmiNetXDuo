#!/usr/bin/env python3
"""Inventory drift and failure-accounting tests; native execution is separate."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
import matrix
import run


class InventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.policy = matrix.validate()
        cls.rows = matrix.matrix(policy=cls.policy)

    def test_every_shipped_executable(self):
        self.assertEqual(set(self.policy), matrix.inventory())
        self.assertEqual(set(self.policy), {r['tool'] for r in self.rows})
        self.assertIn('WirelessManager', self.policy)
        self.assertIn('Profile', self.policy)
        self.assertIn('profspin', self.policy)
        self.assertEqual(len(self.rows), len({r['id'] for r in self.rows}))

    def test_each_option_alias_and_stack(self):
        for tool, spec in self.policy.items():
            rows = [r for r in self.rows if r['tool'] == tool]
            kinds = {r['kind'] for r in rows}
            for k in kinds:
                self.assertEqual({r['stack'] for r in rows if r['kind'] == k}, {4096, 8192})
            if spec['parser'] in ('readargs', 'installer', 'vendored-readargs'):
                for aliases, mods in matrix.fields(matrix.readargs_template(matrix.ROOT, spec)):
                    for alias in aliases:
                        self.assertIn('option:' + alias, kinds)
                    if 'N' in mods:
                        self.assertIn('number:' + aliases[0] + ':2147483648', kinds)
                        self.assertIn('number:' + aliases[0] + ':-2147483649', kinds)
                    if 'A' in mods:
                        self.assertIn('missing-required:' + aliases[0], kinds)
            for k in ('empty', 'help', 'unknown', 'unterminated-quote'):
                self.assertIn(k, kinds)

    def test_inventory_addition_cannot_skip(self):
        with patch.object(matrix, 'inventory', return_value=set(self.policy) | {'NewShippedTool'}):
            with self.assertRaisesRegex(ValueError, 'NewShippedTool'):
                matrix.validate(policy=self.policy)

    def test_option_addition_cannot_skip(self):
        real = matrix.readargs_template
        def added(root, spec):
            template = real(root, spec)
            return template + ',NEWOPTION/K' if spec['source'] == 'src/tools/addnetinterface.c' else template
        with patch.object(matrix, 'readargs_template', side_effect=added):
            with self.assertRaisesRegex(ValueError, 'NEWOPTION'):
                matrix.validate(policy=self.policy)

    def test_quote(self):
        self.assertEqual(matrix.quote('a * " b'), '"a ** *" b"')
        for val in ('a\nb', 'a\rb', 'a\tb', 'a\0b'):
            with self.assertRaises(ValueError):
                matrix.quote(val)

    def test_gui_option_keeps_non_gui_paths_strict(self):
        spec = self.policy['InstallWiFi']
        by_kind = {r['kind']: r for r in matrix.tool_cases('InstallWiFi', spec)}
        self.assertEqual(by_kind['long:TO']['mode'], 'interrupt')
        self.assertEqual(by_kind['option:PRIORITY']['mode'], 'exit')
        self.assertEqual(by_kind['unknown']['mode'], 'exit')

    def test_installer_short_path_failure_stays_covered(self):
        spec = self.policy['Installer']
        by_kind = {r['kind']: r for r in matrix.tool_cases('Installer', spec)}
        row = by_kind['short-path:SCRIPT']
        self.assertEqual(row['arguments'], 'SCRIPT "' + 'x' * 104 + '"')
        self.assertEqual(row['mode'], 'interrupt')

    def test_shards_cover_each_case_once_with_both_stacks(self):
        original = {r['id'] for r in self.rows}
        shards = [run.shard_rows(self.rows, i, 4) for i in range(4)]
        self.assertEqual(original, {r['id'] for shard in shards for r in shard})
        self.assertEqual(len(self.rows), sum(len(shard) for shard in shards))
        for shard in shards:
            self.assertTrue(shard)
            for tool in self.policy:
                self.assertIn(tool, {r['tool'] for r in shard})
            pairs = {}
            for row in shard:
                pairs.setdefault(int(row['id'][1:]) // 2, set()).add(row['stack'])
            self.assertTrue(all(stacks == {4096, 8192} for stacks in pairs.values()))
        for bad_index, bad_count in ((-1, 4), (4, 4), (0, 0)):
            with self.assertRaisesRegex(ValueError, 'shard index'):
                run.shard_rows(self.rows, bad_index, bad_count)


class VerdictTests(unittest.TestCase):
    def setUp(self):
        self.rows = [dict(id='case', stack=4096, mode='exit', allowed=[0, 20])]
        self.good = 'case\tSTART\t0\t4096\t0\ncase\tEND\t0\t4096\t0\nSUITE\tDONE\t0\t0\t0\n'

    def test_positive(self):
        self.assertTrue(run.verdict(self.rows, self.good, 0)[0]['passed'])
        self.assertTrue(run.verdict(self.rows, self.good.replace('END\t0', 'END\t20'), 0)[0]['passed'])

    def test_negative_transcripts(self):
        cases = ('', self.good.split('case\tEND')[0],
                 self.good.replace('END', 'TIMEOUT'), self.good.replace('END', 'INFRA'),
                 self.good.replace('END', 'LATE'), self.good.replace('END\t0', 'END\t42'),
                 self.good.replace('END\t0\t4096', 'END\t0\t65536'),
                 self.good.replace('END\t0\t4096\t0', 'END\t0\t4096\t1'),
                 self.good.replace('SUITE\tDONE\t0\t0\t0', 'SUITE\tDONE\t20\t0\t0'),
                 self.good.replace('SUITE\tDONE\t0\t0\t0', 'SUITE\tDONE\t0\t4096\t0'),
                 self.good.replace('case', 'stale-case'), self.good + self.good,
                 self.good + 'foreign\tEND\t0\t4096\t0\n',
                 self.good + 'garbled\n', self.good.replace('4096', 'nan'),
                 'SUITE\tDONE\t0\t0\t0\n' + self.good.split('SUITE')[0])
        for text in cases:
            with self.subTest(text=text):
                self.assertFalse(run.verdict(self.rows, text, 0)[0]['passed'])
        for rc in (1, 2, 4, 6, 77, 124, -15):
            self.assertFalse(run.verdict(self.rows, self.good, rc)[0]['passed'])
        self.assertFalse(run.verdict(self.rows, self.good, 0, fatal=True)[0]['passed'])

    def test_resident_can_be_stopped(self):
        self.rows[0]['mode'] = 'interrupt'
        text = self.good.replace('END\t0\t4096\t0', 'END\t0\t4096\t1')
        self.assertTrue(run.verdict(self.rows, text, 0)[0]['passed'])

    def test_emulator_fault_and_rom_probe(self):
        probe = 'Illegal instruction: 4e7a at 00F80100 -> 00F80AD2\n'
        fault = 'Illegal instruction: 4afc at 0022E84A -> 00F80AD2\n'
        self.assertFalse(run.guest_fault(probe))
        self.assertFalse(run.guest_fault('child reached TIMEOUT\n'))
        self.assertTrue(run.guest_fault(probe + fault))
        self.assertTrue(run.guest_fault(fault.lower()))
        for message in ('Guru Meditation', 'Software Failure', 'Address Error', 'Bus Error'):
            self.assertTrue(run.guest_fault(message))

    def test_crash_in_middle_preserves_completed_prefix(self):
        rows = [dict(self.rows[0], id=name) for name in ('first', 'crash', 'last')]
        text = ('first\tSTART\t0\t4096\t0\nfirst\tEND\t0\t4096\t0\n'
                'crash\tSTART\t0\t4096\t0\ncrash\tTIMEOUT\t0\t0\t0\n')
        result = run.verdict(rows, text, 124, fatal=True)
        self.assertEqual([r['passed'] for r in result], [True, False, False])
        self.assertEqual(result[1]['reason'], 'fatal emulator/guest exception in batch')
        self.assertEqual(result[2]['events'], [])
        # Truncation after a clean END does not qualify as a killed child.
        truncated = text.split('crash\tSTART')[0]
        self.assertFalse(run.verdict(rows, truncated, 124)[0]['passed'])
        reboot = run.verdict(rows, text.split('crash\tTIMEOUT')[0], 124, rebooted=True)
        self.assertEqual([r['passed'] for r in reboot], [True, False, False])
        self.assertEqual(reboot[1]['reason'], 'guest reboot while child was active')

    def test_late_failure_does_not_hide_following_case(self):
        rows = [dict(self.rows[0], id=name) for name in ('late', 'next')]
        text = ('late\tSTART\t0\t4096\t0\nlate\tLATE\t0\t4096\t1\n'
                'next\tSTART\t0\t4096\t0\nnext\tEND\t0\t4096\t0\n'
                'SUITE\tDONE\t0\t0\t0\n')
        self.assertEqual([r['passed'] for r in run.verdict(rows, text, 0)], [False, True])

    def test_missing_binary_fails(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory(dir=matrix.ROOT / "build") as directory:
            ns = SimpleNamespace(package=None, build=Path(directory), clients=Path(directory))
            with self.assertRaisesRegex(ValueError, 'never skipped'):
                run.binaries(ns, {'Missing': {'binary': 'missing'}})


class OrchestrationTests(unittest.TestCase):
    def test_batch_reads_and_retains_emulator_fault_after_wrapper_exit(self):
        from types import SimpleNamespace
        fault = 'Illegal instruction: 4afc at 0022E84A -> 00F80AD2\n'
        rows = [dict(id='case', tool='CliControl', arguments='CRASH', stack=4096,
                     mode='exit', allowed=[0])]
        with tempfile.TemporaryDirectory(dir=matrix.ROOT / 'build') as directory:
            root = Path(directory)
            (root / 'tests/netstack/devs/NetInterfaces').mkdir(parents=True)
            output = root / 'output'
            (output / 'bin').mkdir(parents=True)
            control = output / 'bin/CliControl'
            control.write_bytes(b'fixture')
            ns = SimpleNamespace(output=output, token='fixture', model='A600')
            tag = 'clicrash-fixture-control-0000'

            class ExitedWrapper:
                returncode = 143

                def __init__(self, *args, **kwargs):
                    hd = root / 'build' / ('amiberry-testhd-' + tag)
                    hd.mkdir(parents=True)
                    (hd / 'cli-results.tsv').write_text(
                        'case\tSTART\t0\t4096\t0\ncase\tTIMEOUT\t0\t0\t0\n')
                    (root / 'build' / ('amiberry-' + tag + '.log')).write_text(fault)
                    kwargs['stdout'].write('wrapper terminated before final assertions\n')

                def poll(self):
                    return self.returncode

            with patch.object(run, 'ROOT', root), patch.object(run.subprocess, 'Popen', ExitedWrapper):
                result = run.batch(ns, rows, {'CliControl': control}, 'control', 0)[0]
            self.assertFalse(result['passed'])
            self.assertEqual(result['reason'], 'fatal emulator/guest exception in batch')
            retained = output / result['emulator_log']
            self.assertEqual(retained.read_text(), fault)
            self.assertEqual(result['emulator_sha256'], run.digest(retained))

    def test_only_unstarted_suffix_is_retried(self):
        from types import SimpleNamespace
        rows = [dict(id=name, stack=4096, mode='exit', allowed=[0]) for name in ('first', 'crash', 'last')]
        calls = []
        def fake_batch(ns, group, paths, state, ordinal):
            calls.append([r['id'] for r in group])
            text = ('first\tSTART\t0\t4096\t0\nfirst\tEND\t0\t4096\t0\n'
                    'crash\tSTART\t0\t4096\t0\ncrash\tTIMEOUT\t0\t0\t0\n') if ordinal == 0 else (
                    'last\tSTART\t0\t4096\t0\nlast\tEND\t0\t4096\t0\nSUITE\tDONE\t0\t0\t0\n')
            return run.verdict(group, text, 124 if ordinal == 0 else 0)
        ns = SimpleNamespace(state=['present'], batch_size=3)
        with patch.object(run, 'batch', side_effect=fake_batch), patch.object(run, 'write_results'), patch('builtins.print'):
            results = run.sweep(ns, rows, {})
        self.assertEqual(calls, [['first', 'crash', 'last'], ['last']])
        self.assertEqual([r['id'] for r in results], ['first', 'crash', 'last'])
        self.assertEqual([r['passed'] for r in results], [True, False, True])

    def test_bootstrap_failure_stops_without_claiming_coverage(self):
        from types import SimpleNamespace
        rows = [dict(id=str(n), stack=4096, mode='exit', allowed=[0]) for n in range(3)]
        ns = SimpleNamespace(state=['present'], batch_size=2)
        with patch.object(run, 'batch', side_effect=lambda n, group, p, s, o: run.verdict(group, '', 124)) as batch, patch.object(run, 'write_results'), patch('builtins.print'):
            results = run.sweep(ns, rows, {})
        self.assertEqual(batch.call_count, 1)
        self.assertEqual(len(results), 3)
        self.assertTrue(all(not r['passed'] and not r['events'] for r in results))


if __name__ == '__main__':
    unittest.main(verbosity=2)
