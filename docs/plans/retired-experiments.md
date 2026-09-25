# Retired experiments (2026-09-25)

These results are retained so dead-end branches can be removed. No code or
bench-only harness from these experiments is enabled or preserved on main.

| Experiment | Result | Decision |
|---|---|---|
| O(1) Exec-wait owner lookup (`claude/perf-owner-lookup`, `2a1049c0`) | The one emulator profile arm reduced the reader-baton total by only 34 samples, below the predeclared 100-sample threshold. The new helper and old short walk both cost 111 samples. | Abandon. Do not carry the OFF-switch code. |
| RTT-tuned cork (`claude/perf-cork-rtt`, `b86966ca`) | No LAN gain over the cork implementation already on main, which itself raised request/response RTT about 11%. | Abandon. Leave the existing cork option default OFF; no more cork runs. |
| Cork A/B and smoke harnesses (`claude/cork-ab`, `claude/cork-smoke`) | No independent product gain; the A/B method is superseded by the `rsgap` harness on main. | Retire these branch-only harnesses with the experiment. |
| Small-write benchmark (`deepseek/perf-smallwrite`, `8185860e`) | Harness only, no measured result. | Retire rather than retaining unvalidated benchmark code. |
| ZZ9000 reset/reclaim test branch (`deepseek/zz9000-reset-reclaim-tests`, `fb51282f`) | Its proposed `tx_next - tx_done == txb_inuse` invariant is wrong on current main: `tx_next` is a window-slot cursor; `tx_done` is an independent firmware-completion counter. Main already tests reset with live slots, completion reclaim and stale-serial recovery under the current semantics. | Superseded; do not merge tests with incorrect expectations. |

ZZ9000 soft-interrupt servicing at `27092b8c` did reduce door latency in a
single A/B arm (p50/p99 327/709 ms to 222/672 ms), but RX throughput fell
from 5.11 to 4.34 Mbit/s. The first gate was void because the Zorro 256 MB
mapping was missing. This is not a demonstrated net performance improvement;
the experimental runtime change remains out of main. The corrected measurement
claims are retained in [zz9000-ethernet.md](zz9000-ethernet.md).
