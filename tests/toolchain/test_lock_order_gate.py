#!/usr/bin/env python3
"""Host regression tests for tools/check-lock-order.py."""

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_lock_order", ROOT / "tools" / "check-lock-order.py"
)
assert SPEC is not None and SPEC.loader is not None
LOCK_ORDER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = LOCK_ORDER
SPEC.loader.exec_module(LOCK_ORDER)


class LockOrderGateTest(unittest.TestCase):
    def analyze_snippet(self, code: str):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "src"
            src.mkdir(parents=True, exist_ok=True)
            (src / "sample.c").write_text(code, encoding="utf-8")
            return LOCK_ORDER.analyze_source_tree(root)

    def test_builtin_selftest_passes(self):
        self.assertEqual(LOCK_ORDER.run_selftest(), 0)

    def test_clean_hierarchy_and_nested_enter_noop(self):
        _, _, analyzer, cycles = self.analyze_snippet(
            """
            void inner_helper(void) {
                AmiNetCaller *c = ami_netstack_enter_alloc();
                ami_netstack_leave_free(c);
            }
            int outer_caller(register struct AmiSocketBase *base __asm("a6")) {
                ObtainSemaphore(&base->sb_Lock);
                if (base->sb_StackRefs == 0) {
                    ReleaseSemaphore(&base->sb_Lock);
                    Forbid();
                    return -1;
                }
                ObtainSemaphore(&bsd_job_lock);
                ami_ns_lock_obtain();
                ami_ns_lock_release();
                ReleaseSemaphore(&bsd_job_lock);
                ReleaseSemaphore(&base->sb_Lock);
                if (bsd_nx_enter(base) == 0) {
                    inner_helper();
                    bsd_nx_leave(base);
                }
                return 0;
            }
            """
        )
        self.assertEqual(cycles, [])
        self.assertEqual(len(analyzer.violations), 0)
        edge_pairs = {(e.from_lock, e.to_lock) for e in analyzer.edges.values()}
        self.assertIn(("sb_Lock", "bsd_job_lock"), edge_pairs)
        self.assertIn(("bsd_job_lock", "ami_ns_lock"), edge_pairs)
        self.assertNotIn(("netx_baton", "adopt_slot"), edge_pairs)

    def test_detects_abba_cycle_and_unbounded_blocking_calls(self):
        _, _, analyzer, cycles = self.analyze_snippet(
            """
            void order_ab(struct AmiSocketBase *base) {
                ObtainSemaphore(&base->sb_Lock);
                ami_ns_lock_obtain();
                ami_ns_lock_release();
                ReleaseSemaphore(&base->sb_Lock);
            }
            void order_ba(struct AmiSocketBase *base) {
                ami_ns_lock_obtain();
                ObtainSemaphore(&base->sb_Lock);
                OpenDevice("cnet.device", 0, req, 0);
                tx_thread_sleep(50);
                ReleaseSemaphore(&base->sb_Lock);
                ami_ns_lock_release();
            }
            static void __attribute__((noinline, cold)) ami_sana2_driver_command(
                NX_IP_DRIVER *driver_req, struct netstack_iface *iface) {
                ami_sana2_block_enter();
                Wait(1U);
                ami_sana2_block_leave();
                tx_semaphore_get(&iface->ready, 50);
            }
            void link_enable(NX_IP *ip) {
                nx_ip_driver_interface_direct_command(ip, NX_LINK_ENABLE, 0, &val);
            }
            """
        )
        self.assertTrue(
            any(set(nodes[:-1]) == {"sb_Lock", "ami_ns_lock"} for nodes, _ in cycles)
        )
        categories = {
            (v.category, v.held_lock, v.triage)
            for v in analyzer.violations.values()
        }
        self.assertIn(("exec_block", "ami_ns_lock", "unbounded"), categories)
        self.assertIn(("unadopted_tx_call", "ami_ns_lock", "unbounded"), categories)
        self.assertIn(
            ("baton_release_with_mutex", "nx_ip_protection", "unbounded"),
            categories,
        )
        self.assertIn(("tx_wait", "nx_ip_protection", "unbounded"), categories)


if __name__ == "__main__":
    unittest.main()
