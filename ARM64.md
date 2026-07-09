# Benchmark overview — arm64

> Darwin 25.5.0, arm64, unpinned. Generated 2026-07-09 by
> `compare2upstream/run.sh` (`min_time=0.3s`,
> `repetitions=10`; cells are means over repetitions). `fork` is
> nanofix; `fork-no-simd` is the same code with `NANOFIX_DISABLE_SIMD`;
> `upstream` is jamesdbrock/hffix. Unpinned numbers are indicative only —
> authoritative A/B comes from a pinned, isolated Linux core
> (`NANOFIX_BENCH_CPU=<cpu>`, governor `performance`, `isolcpus`).

Write/read latency — lower is better; first column is the baseline. Iterator path (`find_with_hint` over a fixed 20-tag set per message); tail percentiles include the ~20-30 ns `clock::now()` probe:

| Benchmark | upstream | fork | fork-no-simd |
| --- | --- | --- | --- |
| `Write p99` | 120.9 ns | **84 ns (-31 %)** | 84 ns (-31 %) |
| `Write p999` | 158.2 ns | **92.2 ns (-42 %)** | 92.2 ns (-42 %) |
| `Read seq p99` | 85904 ns | **59175 ns (-31 %)** | 59354 ns (-31 %) |
| `Read seq p999` | 96979 ns | 64063 ns (-34 %) | **62817 ns (-35 %)** |
| `Read rand p99` | 125084 ns | 88358 ns (-29 %) | **87233 ns (-30 %)** |
| `Read rand p999` | 149321 ns | 112200 ns (-25 %) | **91171 ns (-39 %)** |

Write/read throughput — higher is better; iterator path, like-for-like, first column is the baseline:

| Benchmark | upstream | fork | fork-no-simd |
| --- | --- | --- | --- |
| `Write (NewOrder)` | 13.2 M msgs/s | 19.8 M msgs/s (+50 %) | **20.3 M msgs/s (+54 %)** |
| `Read seq` | 30.9 k msgs/s | 44.1 k msgs/s (+43 %) | **44.2 k msgs/s (+43 %)** |
| `Read random` | 21.4 k msgs/s | **30.3 k msgs/s (+42 %)** | 29.8 k msgs/s (+40 %) |

New access path vs the upstream iterator — latency (ns), lower is better. Same 20-tag workload as the throughput table below; reading many tags makes the iterator rescan per lookup, while `build_field_index` is O(length) regardless of lookup count. Shown SIMD-on (`fork`) and SIMD-off (`fork-no-simd`). Tail percentiles include the ~20-30 ns `clock::now()` probe:

| Benchmark | upstream iterator | fork indexed | fork-no-simd indexed |
| --- | --- | --- | --- |
| `Read seq p99` | 85904 ns | **4704 ns (-95 %)** | 11967 ns (-86 %) |
| `Read seq p999` | 96979 ns | **6075 ns (-94 %)** | 13292 ns (-86 %) |
| `Read rand p99` | 125084 ns | **4958 ns (-96 %)** | 13221 ns (-89 %) |
| `Read rand p999` | 149321 ns | **5471 ns (-96 %)** | 14604 ns (-90 %) |

New access path vs the upstream iterator — message throughput, higher is better, delta vs upstream's iterator on the same workload. Shown SIMD-on (`fork`) and SIMD-off (`fork-no-simd`):

| Benchmark | upstream iterator | fork indexed | fork-no-simd indexed |
| --- | --- | --- | --- |
| `Read seq` | 30.9 k msgs/s | **538 k msgs/s (+1639 %)** | 217 k msgs/s (+603 %) |
| `Read random` | 21.4 k msgs/s | **506 k msgs/s (+2269 %)** | 193 k msgs/s (+802 %) |

Index amortization — how many `find()`s per message before `build_field_index` + indexed lookups beat the iterator (`find_with_hint`), same binary, same messages (per-message ns). Break-even interpolated from `BM_Parse_FindN_{Iter,Indexed}`; below it, iterate — above it, index:

| Config | Break-even | iter @3 | indexed @3 | iter @30 | indexed @30 |
| --- | --- | --- | --- | --- | --- |
| `fork` | **~6.4 finds/msg** | 22.77 ns | 1558 ns | 42812 ns | 2001 ns |
| `fork-no-simd` | **~10.6 finds/msg** | 22.67 ns | 3469 ns | 43282 ns | 5450 ns |

