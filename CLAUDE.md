# CLAUDE.md

Project-level guidance for Claude.

## What this is

`nanofix` is a header-only FIX 4.x/5.0 parser. Fork of
`jamesdbrock/hffix`. Scope: serialize + deserialize FIX wire format.
Nothing else. No transport, no session layer, no business logic.

## Hard rules

1. **Parser only.** Do not add session-state, sequence-number tracking,
   reconnect logic, persistence, or any networking. If a feature
   conceptually belongs in QuickFIX/OnixS, it does not belong here.
2. **Zero-alloc end-to-end.** No `std::string`, `std::vector`, `new`,
   `malloc` on any reader or writer path. Buffer is always
   caller-supplied. Views are spans/string_views into that buffer.
3. **No exceptions.** Reader and writer methods are
   `noexcept`. Errors surface via return code or sticky flag;
   programmer-error invariants use `NANOFIX_ASSERT` (see
   `NANOFIX_ASSERT` below).
4. **Header-only.** Hand-written code lives in `include/nanofix/detail/*.hpp`
   (split by area: `config`, `diagnostics`, `simd`, `numeric`, `time`, `writer`,
   `value_iter`, `reader`, `index`, `typed`); `include/nanofix.hpp` is the umbrella that
   includes them. Each internal header pulls its own dependencies, so umbrella
   include order does not matter. The generated `include/nanofix/detail/fields.hpp`
   (typed `tag::` handles) also sits under `detail/` but is machine-generated —
   do not hand-edit; `include/nanofix/names.hpp` carries the opt-in name tables.
   No `.cpp` for library code.
   Repeating groups are read only through the runtime `reader.group(count_tag,
   delimiter_tag)` overload (caller supplies the delimiter); there is no
   compile-time `group<>` dispatch — a group's delimiter can differ per MsgType.
5. **C++20.** Do not gate features on older standards.

## `NANOFIX_ASSERT`

One path independent of `NDEBUG`: a failed assert bumps an atomic counter
(`nanofix::assert_failure_count()`) and runs any installed handler
(`set_assert_handler`), then returns. Build with
`-DNANOFIX_ASSERT_FAILFAST` to also trap after the handler — the
benchmarks and fuzz harness do, so a tripped invariant aborts (the
fuzzer needs the signal to flag it; benches must not produce skewed
timings). `NANOFIX_ASSERT` is defined unconditionally (no `#ifndef` guard);
customize the failure behavior at runtime via `set_assert_handler`, not by
redefining the macro. Tests assert against the counter, not a trap.

## Hot-path conventions

- Annotate hot functions: `NANOFIX_HOT NANOFIX_ALWAYS_INLINE`.
- Mark error/rare branches `[[unlikely]]`. Mark predictable common
  branches `[[likely]]` only when profiling justifies it.

## Changing the hot path

Any change to a hot scan/parse function (`build_field_index`,
`find_soh` / `find_all_soh`, iterator
`increment`, checksum) is an *experiment*, not a commit. Gate it in order:

1. **Characterization first.** Before editing, keep the current
   implementation as a reference in the test and assert the new path is
   byte-identical over every `tests/data/` fixture, the malformed
   fixtures, and the truncation boundaries — `BulkFramingDiffTest`
   (`tests/framing_diff_tests.cpp`) is the template: it diffs
   `(tag, pos, len, truncated)` from a per-field reference against the
   live `build_field_index`. A faster path that changes output is a
   regression, not an optimization.
2. **A/B benchmark, controlled.** Build two binaries (old vs new), run
   them alternating on an otherwise-idle machine — no fuzzer, no build,
   nothing else on the cores. Compare medians over ≥10 repetitions, and
   always include a *control* benchmark whose code you did not touch
   (e.g. `BM_Parse_IterAllFields` for an indexed-path change). If the
   control moves more than ~1–2%, the run is confounded (thermal / code
   layout / background load) — discard and re-run. The first A/B is
   usually confounded for exactly this reason.
3. **Fuzz.** Survive one full `NANOFIX_FUZZ_MAX_TIME` budget with no crash;
   add a regression test for anything it finds (see ## Fuzzing).
4. **Keep only on a measured end-to-end win.** Neutral or negative →
   revert and report the numbers. Do not merge a hopeful change.

Measure each call site, not the primitive. A trick that wins on a
scan-everything path can lose on an early-exit one: the bulk `find_all_soh`
sweep gave `build_field_index` ~+28% (it indexes every field) but lost on
early-exit callers that stop once the wanted tags are seen, where eager
whole-message framing is wasted work — so those keep a per-field scalar scan
(`find_soh`).

### Measurement environment

Google Benchmark does **not** pin the workload to a core — its only
affinity touch is a transient probe for the CPU-frequency *metadata*
(hence the `Failed to set thread affinity` warning, which does not affect
timings). Pinning + isolation are the caller's job, and on a non-pinned
host run-to-run noise routinely swamps a real 1–2% hot-path delta.

Authoritative A/B numbers come from **Linux**, pinned and isolated:

- Pin to one physical core: `taskset -c <cpu> ./bench` (or
  `numactl --physcpubind=<cpu> --membind=<node>`). `compare2upstream/run.sh`
  does this automatically on Linux when `NANOFIX_BENCH_CPU` is set (e.g.
  `NANOFIX_BENCH_CPU=3`), and the same core is reused across all configs so
  the comparison stays apples-to-apples.
- Isolate that core from the scheduler: boot with `isolcpus=<cpu>
  nohz_full=<cpu> rcu_nocbs=<cpu>`, or `cset shield -c <cpu>`. Pin the
  bench to an *isolated* core, not just any core.
- Kill frequency variance: governor `performance`
  (`cpupower frequency-set -g performance`), disable turbo
  (`echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo`), and disable
  the SMT sibling of the chosen core.
- Real-time priority optional: `chrt -f 80 taskset -c <cpu> ./bench`.
- Code-layout variance: bench targets compile with the JCC-erratum
  mitigation (`cmake/nanofix-jcc.cmake`, branch padding off 32B boundaries
  on x86-64) — without it, unrelated edits to always-inline headers flip
  multi-percent penalties on Skylake-family hosts between binaries.

**macOS has no hard pinning** — `thread_policy_set` affinity tags are
hints the scheduler may ignore, and Apple Silicon P/E-core migration adds
its own variance (the workload can land on an E-core mid-run). Treat macOS
numbers as *indicative only*: medians over ≥10 reps with a control bench,
never the basis for keeping a sub-5% hot-path change. Promote anything
marginal to a pinned Linux run before it ships.

## Buffer and string types

The public boundary takes `std::span<char const>` / `std::span<char>` and
`std::string_view`; the `(begin, end)` and `(ptr, size)` ctors delegate to
the span ctor. The hot path walks raw `char const*` internally. span and
string_view are `{ptr, len}` views over the same bytes (no overhead), so
char* stays inside and span/string_view sit at the boundary. `field_value`
exposes `bytes()` (`span<char const>`) and `as_string_view()`; `begin()`/
`end()` are for iteration. New public API keeps this split: do not expose
raw `char*` pairs as the only form.

## SIMD

`find_all_soh`, `find_tag_in_index`, `checksum_bytes` ship three impls
(scalar / NEON / AVX2). Dispatch is a compile-time `#if/#elif/#else`
on `NANOFIX_HAS_AVX2` / `NANOFIX_HAS_NEON`. Architecture baselines:
AVX2 on x86-64 (Haswell+, 2013), NEON on aarch64 (ARMv8 mandatory).
Consequence: x86-64 binaries require Haswell+ — use
`NANOFIX_DISABLE_SIMD` for pre-2013 deploys. There is no vector SOH
search: every per-field caller is a serial scan of a short FIX value
where a vector load's lane extract loses to scalar byte compares (a
vector path at the iterator-increment scan regressed `BM_ReadMessageScan`
~17%), so the scalar `find_soh` lives in `value_iter.hpp` (next to
`scan_tag_digits`), not here. Bulk whole-message framing vectorizes
through `find_all_soh`.

AVX2 is a hard baseline, enforced globally, not per function: on x86-64
`simd.hpp` `#error`s unless `__AVX2__` is defined (or
`NANOFIX_DISABLE_SIMD=1`), and the build propagates `-mavx2` /
`/arch:AVX2` itself — the `nanofix` CMake target (build interface),
`nanofix-config.cmake.in` (installed consumers, applied at their
configure), Conan `package_info()` (`cxxflags`), and `fuzz/CMakeLists.txt`
each add it for x86-64. There is no per-function
`__attribute__((target("avx2")))`: with the flag global, the `*_avx2`
impls are `NANOFIX_ALWAYS_INLINE` like every other variant (a `target`
attribute would forbid inlining into non-`target` callers). The vector
checksums fall back to scalar on short input (`checksum_bytes_avx2` below
256 B, `checksum_bytes_neon` below 1024 B) — a threshold change is a
hot-path experiment like any other.

When adding a SIMD path: define `*_scalar`, `*_avx2`, `*_neon`, all
`NANOFIX_ALWAYS_INLINE`, dispatch from a single `NANOFIX_ALWAYS_INLINE`
wrapper via `#if/#elif/#else`, always provide
scalar fallback. `NANOFIX_DISABLE_SIMD=1` collapses everything to scalar
— keep that path compiling on every change; CI's `scalar` job builds and
tests it on every push.

## `is_known_tag(int)`

Bitmap of every tag in the loaded FIX spec. 782 × uint64 constexpr,
~6 KB, fits L1D. Parser doesn't range-check tag values (1-2% hot-loop
cost); whitelist call filters garbage.

```cpp
for (auto it = r.begin(); it != r.end(); ++it) {
    if (!nanofix::is_known_tag(it->tag())) continue;
    dispatch(it->tag(), it->value());
}
```

Append venue XML to `fixspec-gen` input to union the bitmap.

## Validating vs unchecked parsers

- `try_as_int(T&)` / `try_as_decimal(T&, T&)`: validating (digits and
  overflow), `noexcept`, returns `bool`.
- `as_int_unchecked<T>()` / `as_decimal_unchecked<T>()`: garbage in =
  undefined output. Suffix is mandatory consent.
- Default new code to the `try_*` variants. Use `_unchecked` only when
  the caller has externally validated the field.
- `try_as_char(char&)`: validating single-byte read for `char` fields (rejects
  empty and any length != 1). Prefer over `as_char_unchecked()` (reads
  `*begin()` blind — UB on empty, silent truncation on multi-byte) for anything
  off the wire.
- `try_as_bool(bool&)` / `as_bool_unchecked()`: FIX Boolean (`'Y'` = true,
  `'N'` = false). The `try_*` form validates (rejects other bytes / wrong
  length); the unchecked form is first-byte `== 'Y'`.
- `field_value::try_as<T>(T&)` / `as_unchecked<T>()`: `if constexpr` facades
  over the named methods. `T` is an integral, `bool` (matched before integral →
  Boolean-decodes, not `0`/`1`), `char`, `std::string_view`,
  `decimal_parts<Int>` (plain `{mantissa, exponent}` carrier, `Int` defaults
  `int64_t`, no operators) routing to `try_as_decimal`, or a chrono type:
  `std::chrono::time_point` (UTCTimestamp), `duration` (UTCTimeOnly),
  `year_month_day` (date), `year_month` (MonthYear). Chrono types route to the
  named `try_as_timestamp` / `try_as_timeonly` / `try_as_date` /
  `try_as_monthyear` — fully validating (digits/separators/calendar incl.
  day-in-month/clock ranges — stricter than the length-only parts-based
  `as_*`); sub-milli targets read the nano wire
  formats, milli-or-coarser targets reject sub-ms wire instead of truncating.
  Everything that produces an epoch or `time_point` (`as_epoch_millis` /
  `as_epoch_nanos`, `as_timestamp(tp)` / `as_timestamp_nano(tp)`) is fully
  validating too — a wrong-but-plausible epoch never comes back; only the
  parts-based `as_*` (ints out) stay length-only.
  No `try_as_fixed`:
  decimal→fixed-point ticks is consumer/venue knowledge (no FIX wire type is
  fixed-point), so it lives in the caller.

## Typed tags (the default read API)

Each `tag::X` is a `field_tag<Tag, fix_type>` (in `detail/typed.hpp`). It carries
the field's FIX type *category* (`decimal` / `integer` / `string` / `character` /
`timestamp` / `timeonly` / `date` / `monthyear` / `unknown`) and converts to its `int` number via a `constexpr
operator int()`, so writer / comparison / `group()` call sites take it unchanged.

`find(tag::X)` and `find_with_hint(tag::X, cur)` on `message_reader`,
`group_entry`, `indexed_message`, `indexed_fields`, `iter_fields` return a
`typed_value<Type>`:

- Universal (any category): `bytes()`, `as_string_view()`,
  `empty()`, `value()`, `explicit operator bool` (truthy = found).
- Category-gated (compile error otherwise): `try_as_int`, `try_as_decimal`,
  `try_as_char` / `try_as_bool` (`character` — single byte; bool is `'Y'`/`'N'`),
  `as_timestamp[_nano]` / `as_epoch_*` / `try_as_timestamp` (`timestamp`),
  `as_timeonly[_nano]` / `try_as_timeonly` (`timeonly`), `as_date` /
  `try_as_date` (`date` — LocalMktDate/UTCDateOnly), `as_monthyear` /
  `try_as_monthyear` (`monthyear`). The `try_as_*` chrono forms and the
  epoch/`time_point`-producing `as_*` forms are the fully validating tier
  (digits/separators/calendar/clock ranges); the parts-based `as_*` forms
  validate length only. Wrong-type access does not compile.
- No `_unchecked` on `typed_value`: reach the ungated, any-tag escape hatch via
  `.value()` (returns the raw `field_value`, which keeps the full ungated API).

Gating is all `requires` (compile-time) and `typed_value` is a `field_value` +
phantom type — **zero runtime overhead** when the tag is known at compile time.
`field_value` (from `it->value()` / the streaming loop / runtime-`int`
`find_with_hint(int, …)`) stays the raw tier with the full ungated surface. When
a test or example reads a field, use the tag's real category (e.g. `OrderQty`,
`Price`, `MDEntrySize` are `Qty`/`Price` = decimal → `try_as_decimal`), not the
escape hatch.

## Build

Toolchain: CMake 3.20+, Conan 2.x, C++20.

```sh
pip install 'conan>=2.0,<3.0'
conan profile detect --force

conan install . --output-folder=build --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20
cmake --preset conan-release
cmake --build build/build/Release -j
```

`conan install` puts the build tree under `build/build/Release` (the
`conan-release` preset's binary dir), so all `cmake --build` / `ctest` /
binary paths below are under it. A local checkout is the top-level CMake
project, so `NANOFIX_BUILD` defaults ON and tests/benches/CLI build with no
extra flag; the `conanfile` forces them off only when nanofix is packaged as a
dependency (`conan create`, via its `build()` method).

Binaries land in `build/build/Release/utils/` (`fixprint`, `fixgen`,
`fixspec-gen`) and `build/build/Release/benchmarks/nanofix_benchmarks`.

Useful CMake options (defaults in parens):

| Option | Default | Effect |
| --- | --- | --- |
| `NANOFIX_BUILD` | ON top-level | Builds tests, benches, CLI utils. |
| `NANOFIX_BUILD_FIXSPEC_GEN` | matches `NANOFIX_BUILD` | Builds `fixspec-gen` (needed by `nanofix_generate`). |
| `NANOFIX_NATIVE_ARCH` | ON | `-march=native` / `-mcpu=native`. |
| `NANOFIX_LTO` | ON | `CMAKE_INTERPROCEDURAL_OPTIMIZATION`; Clang/AppleClang forced to `-flto=full`. |
| `NANOFIX_SANITIZE` | OFF | ASan + UBSan. Use with `-DCMAKE_BUILD_TYPE=Debug` and `-DNANOFIX_LTO=OFF`. |
| `NANOFIX_SANITIZE_THREAD` | OFF | TSan. Mutually exclusive with `NANOFIX_SANITIZE`. |
| `NANOFIX_BENCH_MESSAGES` | 500000 | Synthetic dataset size for `bench_data`. |
| `NANOFIX_BENCH_MIN_TIME` | `1s` | `--benchmark_min_time` for `bench` target. |
| `NANOFIX_BENCH_REPETITIONS` | 5 | `--benchmark_repetitions` for `bench` target. |

## Test

```sh
ctest --test-dir build/build/Release --output-on-failure
```

Tests are split by area: `tests/unit_tests.cpp` (core API surface incl.
`IsKnownTag.*`), `tests/typed_tests.cpp` (typed-tag `find`/`typed_value`
gating), `tests/state_machine_tests.cpp` (`StateMachineCorruption` reader
robustness), `tests/integration_tests.cpp` (round-trip + malformed-input
fixtures from `tests/data/`), `tests/framing_diff_tests.cpp`
(`BulkFramingDiffTest`: the bulk-SIMD `build_field_index` must match the
per-field reference byte-for-byte), and `tests/threading_tests.cpp`
(TSan-validated concurrent-reader claims from the README "Thread safety and
errors"
section). `tests/test_common.hpp` holds the shared fixture loader. All run as a
single `tests` binary via GoogleTest. Run after every header change.

## Sanitizers

```sh
conan install . --output-folder=build-asan --build=missing \
    -s build_type=Debug -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20
cmake --preset conan-debug -DNANOFIX_SANITIZE=ON -DNANOFIX_LTO=OFF -DNANOFIX_NATIVE_ARCH=OFF
cmake --build build-asan/build/Debug -j
ctest --test-dir build-asan/build/Debug
```

Use ASan/UBSan whenever touching reader state machines (`init()`,
`increment()`, `next_message_reader()`) or pointer arithmetic guards.

## Fuzzing

`fuzz/` is a standalone CMake project. Apple Clang lacks
`libclang_rt.fuzzer`; use Homebrew LLVM:

```sh
cmake -S fuzz -B build/fuzz \
    -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/fuzz --target fuzz
```

`-DNANOFIX_FUZZ_MAX_TIME=<sec>` (a CMake var, set at configure) overrides
the default 60s budget baked into the `fuzz` target. The
`fuzz` target depends on `fuzz_dataset`, which copies `tests/data/*`
into `build/fuzz/dataset/` at build time. libFuzzer mutates seeds and
persists new coverage-relevant inputs into the same directory; the
build tree is gitignored so nothing leaks into the repo. Crashes are
written next to the working directory; replay with
`build/fuzz/fuzz_reader ./crash-<hash>`.

For a long campaign, `scripts/gcloud_fuzz.py --project <p> [--time 3600]`
runs fuzz_reader on a fresh GCE VM with one job per vCPU, prints per-job
coverage/stats to stdout, fetches any artifacts to `./fuzz-artifacts/`
(exit non-zero when there are any), and deletes the VM.

`fuzz/fix.dict` is a libFuzzer dictionary of FIX-shaped tokens (SOH,
header/trailer tag prefixes, BeginString variants, MsgType, data-length
pairs, group count tags, timestamp formats, minimum frames). Wired via
`-dict=...` on the fuzz target. Extend it when adding fuzz coverage to
a new tag family.

`fuzz/fuzz_reader.cpp` exercises 18 group types (NoMDEntries, NoPartyIDs,
NoOrders, NoLegs, NoSides, NoTrades, NoAllocs, NoContraBrokers,
NoMiscFees, NoRelatedSym, NoQuoteEntries, NoExecs, NoFills, NoEvents,
NoInstrAttrib, NoSecurityAltID, NoUnderlyings, NoTradingSessions) via
the runtime-dispatched `r.group(count_tag, delim_tag)` overload plus
`build_field_index(entry, ...)` on every encountered entry. Add to the
`kGroups[]` table in that file to cover more.

Add a regression test in `tests/unit_tests.cpp` for every crash the
fuzzer finds, then re-run the harness until the dataset survives at
least one full budget without hits.

## Synthetic dataset (`benchmarks/data/synthetic.fix`)

The dataset is NOT committed (multi-GB at default size). Generate
locally:

```sh
cmake --build build/build/Release --target bench_data
```

Writes `benchmarks/data/synthetic.fix` with `NANOFIX_BENCH_MESSAGES`
messages (default 500000) using `utils/fixgen`. `fixgen` is
deterministic: seeded `std::mt19937` (default seed 42 via `-s`),
fixed message-type mix (MD snapshots, MD incrementals,
NewOrderSingle, ExecutionReport, Logon/Logout/Heartbeat).

Re-run `bench_data` only when:
- the generator code changed,
- `NANOFIX_BENCH_MESSAGES` changed,
- the seed changed,
- spec headers were regenerated and field offsets shifted.

Otherwise the existing file is reused across bench runs.

Direct invocation if needed:
```sh
build/build/Release/utils/fixgen -o benchmarks/data/synthetic.fix -n 500000 -s 42
```

## Benchmarks

```sh
cmake --build build/build/Release --target bench
```

`bench` does NOT depend on `bench_data`; missing dataset = the bench
emits a skip. Bench parameters come from cache vars
(`NANOFIX_BENCH_MIN_TIME`, `NANOFIX_BENCH_REPETITIONS`). The binary is
`build/build/Release/benchmarks/nanofix_benchmarks`; invoke directly for custom
Google Benchmark flags.

## Compare against upstream

Differential bench vs `jamesdbrock/hffix`:

```sh
compare2upstream/run.sh
```

What it does:
1. Clones upstream to `compare2upstream/upstream-src/` (cached).
   Pin via `NANOFIX_UPSTREAM_REF=<tag>` env.
2. Generates `benchmarks/data/synthetic.fix` if missing.
3. Builds three configs into
   `compare2upstream/build/{upstream,fork,fork-no-simd}/`:
   - `upstream`: standalone CMake project at `compare2upstream/upstream-benchmarks/`
     building `upstream_benchmarks` against
     `compare2upstream/upstream-benchmarks/upstream_benchmarks.cpp` (only uses upstream
     API surface). Configured with `-DNANOFIX_UPSTREAM_INCLUDE_DIR=...`
     pointing at the cloned upstream-src include dir.
   - `fork`: this repo, default flags (SIMD on).
   - `fork-no-simd`: this repo with `-DNANOFIX_DISABLE_SIMD=ON`. Isolates
     SIMD's contribution from the rest of the fork's changes.
4. Runs the binaries in config-alternating rounds (A,B,C, A,B,C, …,
   `NANOFIX_BENCH_REPETITIONS` rounds, one repetition per invocation) so
   slow host drift lands evenly on every config, with a
   `--benchmark_filter` that selects **only the benches the overview table
   consumes** (`BENCH_FILTER` in `run.sh`) — not the whole suite. Writes
   per-round JSON to `compare2upstream/results/`.
5. `compare2upstream/render.py` prints the tables (see below).

Override URL / ref / timing via env: `NANOFIX_UPSTREAM_URL`,
`NANOFIX_UPSTREAM_REF`, `NANOFIX_BENCH_MIN_TIME`,
`NANOFIX_BENCH_REPETITIONS`.

### Rendered tables

`render.py` prints a **small README overview**: five compact tables
(render.py's default `--tables` selection — latency, throughput,
newapi-latency, newapi-throughput, amortization): write/read latency
(p99/p999) and throughput on the iterator path; the new access path
(`build_field_index`) vs the upstream iterator as both latency (p99/p999)
and throughput, shown SIMD-on (`fork`) and SIMD-off (`fork-no-simd`); and
the index amortization break-even — the per-message `find()` count at which
`build_field_index` + indexed lookups overtake the iterator, interpolated
from `BM_Parse_FindN_{Iter,Indexed}`. Write throughput is derived in
`render.py` as `1e9 / cpu_time` from the single-message `BM_WriteNewOrder`.
It is deliberately NOT the full per-path dump — detailed analysis comes
from the raw `benchmarks/` GB output.

Cells are medians across the rounds; a rep-to-rep `cpu_time` spread above
2% on any bench prints a confounded-run warning to stderr. Tail
percentiles include the per-message latency probe (RDTSCP on x86-64,
steady_clock elsewhere); both suites carry the identical probe so its cost
cancels from the comparison.

### Committed snapshot and GCE

Tables go to stdout, progress to stderr; the committed snapshot at the
repo root (`X86_64.md`, linked from the README's "Indicative numbers"
section) is captured by redirecting: `compare2upstream/run.sh > X86_64.md`.
`scripts/gcloud_compare2upstream.py` runs the same harness on a fresh GCE
x86-64 VM (SMT off, bench core isolated via `isolcpus`/`nohz_full`/
`rcu_nocbs` + reboot, pinned to it, VM deleted afterwards) and prints the
tables the same way:
`scripts/gcloud_compare2upstream.py --project <p> > X86_64.md`.

### Adding benches

`compare2upstream/upstream-benchmarks/upstream_benchmarks.cpp` is intentionally limited
to upstream's API (no `try_as_int`, no indexed message; iterator + writer
only). It lives in a standalone CMake project (`compare2upstream/upstream-benchmarks/`)
that does NOT depend on the main repo's CMakeLists; it links against upstream's
headers via `NANOFIX_UPSTREAM_INCLUDE_DIR`. The fork-only indexed access path
has no upstream counterpart, so `render.py` compares
it against upstream's iterator. If a bench needs new fork API, add it ONLY to
the fork suite, which is split by theme: `benchmarks/bench_write.cpp`
(serialization), `benchmarks/bench_primitives.cpp` (SIMD/micro + single-message
read), `benchmarks/bench_parse.cpp` (dataset parse paths + `main`), with shared
message builders in `benchmarks/bench_common.hpp`. Each TU defines its benches in
an anonymous namespace plus a `register_*()` that `main` calls; add a new bench
to the matching theme and register it there. Any bench added to BOTH binaries for
the comparison (e.g. write tail latency, per-entry group read) must use only
upstream's API on that side and carry the same name, and be added to
`BENCH_FILTER`.

## Generated headers

`fixspec-gen` emits two headers under fixed names into the `-d` directory
(`fields.hpp` into its `detail/` subdir, `names.hpp` at the root):

- `include/nanofix/detail/fields.hpp` (~6500 lines): do NOT edit by hand. The `tag::`
  namespace is one `field_tag<Tag, fix_type>` object per tag (typed handle with
  an implicit `operator int()`), plus `length_fields`, the `is_known_tag`
  bitmap, and `msg_type` constants. `field_tag` / `fix_type` themselves are
  hand-written in `detail/typed.hpp` (included here to break the cycle with
  `value_iter.hpp`).
- `include/nanofix/names.hpp` (~11800 lines): do NOT edit by hand. Opt-in
  human-readable names: the constexpr lookups `field_name` (switch),
  `msg_type_name` (linear), `value_name` (binary search) — each strategy fit to
  its key cardinality — plus the `dictionary_init_field` /
  `dictionary_init_message` builders, which loop over those same tables to fill
  a caller's map rather than re-emitting the name data. Not included by
  `nanofix.hpp`, so it costs nothing unless a TU includes it.

Regenerate after any spec change:

```sh
cmake --build build/build/Release --target fixspec-gen
build/build/Release/utils/fixspec-gen fixspec/FIX50SP2.xml fixspec/FIXT11.xml \
    -d include/nanofix
```

FIX 5.0 needs both inputs (`FIX50SP2.xml` for app messages, `FIXT11.xml` for
session; the latter also supplies the header/trailer fields). Extra venue specs
append as more positional args; conflicts on the same tag resolve as first-wins
for delimiters, alias for name clashes. Commit XML + generated headers together.

Downstream projects can regenerate at build time via
`nanofix_generate(TARGET ... SPEC_XML ...)` in `CMakeLists.txt`. See
`examples/fix44_gateway/`.

## Versioning

Version truth is **git**: the latest reachable `v*` tag via `git describe`.
`v1.2.3` → `1.2.3`; commits after the tag → `1.2.3+5.gabc1234` (git suffix as
semver build metadata, so Conan version ranges still match the base); no tag →
`0.0.0+g<sha>` with a warning. Releasing = `git tag -a vX.Y.Z && git push
--tags`.

The real `version.hpp` (the `NANOFIX_VERSION*` macros) is **generated into the
build tree** at configure time
(`<build>/nanofix-generated/include/nanofix/detail/version.hpp`); the
committed `include/nanofix/detail/version.hpp` is a `0.0.0+unknown` **stub**
for builds that bypass CMake. The generated dir sits before the source
include dir on every target (`NANOFIX_VERSION_INCLUDE_DIR` from
`cmake/nanofix-version.cmake`), so the generated header shadows the stub, and
`cmake --install` ships the generated one in place of the stub. Never write a
real version into the stub.

Two implementations of the derivation, one header template:
`cmake/nanofix-version.cmake` (runs before `project()`; also included by the
standalone `fuzz/` project; pure CMake because configure must work with no
shell or Python installed) and `scripts/version.py` (`git_version()` +
`render_header()`; doubles as the CLI — `--print` / `--check <header>` /
`<output>` write; never touches the stub). CI's `version` job cross-checks
the two. Both render `cmake/version.hpp.in` — the only place the header text
lives. `conanfile.py` has no derivation of its own: it loads
`scripts/version.py` (exported next to the recipe via `exports`) for
`set_version()`, passes the resolved version into the cache build as
`-DNANOFIX_VERSION_OVERRIDE` (the Conan source copy has no `.git`), and its
`export_sources()` stamps the rendered header over the stub in the exported
source copy, so the export is self-contained even before the CMake install
regenerates it. The examples' `conanfile.txt` require `nanofix/[>=0.0.0]` — a
fixed pin would only resolve on tagged commits.

## Formatting

**After every code change, before calling it done: run `scripts/format.sh`
then `scripts/lint.sh <build-dir>` (clang-tidy) and fix what they report.**
Not optional, not just before commit — every edit to `include/`, `utils/`,
`tests/`, or `benchmarks/`. A change that hasn't been formatted and tidied is
not finished.

```sh
scripts/format.sh        # rewrite in place
scripts/format_check.sh  # dry-run, exits non-zero on diff (CI)
```

Both honor `.clang-format-ignore`. `_sources.sh` enumerates the file
list (`include/`, `utils/`, `tests/`, `benchmarks/`, `fuzz/`,
`compare2upstream/upstream-benchmarks/`); do not run `clang-format` directly on
`compare2upstream/upstream-src/`, its `build/` / `results/`, or the generated headers.

## clang-tidy

```sh
cmake --preset conan-release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
scripts/lint.sh build/build/Release   # build dir holding compile_commands.json
```

`.clang-tidy` runs bugprone/performance/portability + the static analyzer
as errors (`WarningsAsErrors: '*'`). `HeaderFilterRegex` scopes header
diagnostics to `include/nanofix.hpp` and the hand-written `detail/` headers,
enumerated by name (`config|diagnostics|simd|numeric|time|writer|value_iter|reader|index|typed`)
so the generated `detail/fields.hpp` and `nanofix/names.hpp` are not linted —
clang-tidy's regex has no negative lookahead, so the allow-list is explicit. The script lints every `.cpp` under
`utils/`, `tests/`, and `benchmarks/`; checks that fight the design
(no-exceptions callbacks, raw-pointer walking, SIMD `reinterpret_cast`,
caller-owned C arrays, perf int/size_t casts) are disabled in `.clang-tidy`.
CI runs the same in the `clang-tidy` job.

## Tests are part of the API contract

Every public API change must update `tests/unit_tests.cpp`. Bounds
behavior changes go in `tests/integration_tests.cpp`. If a test fails
after a change, the change is wrong by default; do not loosen the test
to make it pass without understanding why it failed.

## API_CHANGES.md

`API_CHANGES.md` is the porting guide: it documents every public-API
difference from upstream `jamesdbrock/hffix` (exceptions and `std::string`
removed, Boost dropped, checked/unchecked parse split, indexed reads). Any
public API change updates it alongside `tests/unit_tests.cpp` — new surface in
"Smaller changes" / "Later additions in this fork", removals/renames where
upstream had a counterpart.

## Style

- `.clang-format` is authoritative. Run `scripts/format.sh` before
  committing.
- Comments: only where the "why" is non-obvious. Do not narrate the
  code. No decorative separators (`// === foo ===`).
- Doxygen blocks only on public types/methods, and only when they say
  something the signature does not.

## What not to do

- Do not add Boost. Boost.DateTime was removed intentionally; `chrono`
  + raw epoch ints are the only time API.
- Do not add `as_string()` back (returned `std::string`, allocated).
- Do not throw from any reader or writer method.
- Do not silently expand `field_index_buffer<N>` capacity. Caller
  controls `N`; overflow is a documented failure mode (`truncated()`,
  which drops the index to zero usable fields — all-or-nothing).
- Do not edit `compare2upstream/upstream-src/`; that tree is a
  pinned snapshot of upstream for differential benchmarking.
