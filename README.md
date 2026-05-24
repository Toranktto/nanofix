# nanofix

A header-only C++20 FIX 4.x/5.0 parser built for low-latency systems, forked
from [jamesdbrock/hffix](https://github.com/jamesdbrock/hffix). Every read and
write path allocates nothing, throws nothing, copies nothing — you own the
buffer, the reader views it, the writer fills it. SIMD (AVX2/NEON) drives the
hot scan, checksum, and indexed tag lookup; the tail percentiles are in the
[benchmark tables](#benchmarks) below.

Tags carry their FIX type at compile time. `find(tag::Price)` returns a
`typed_value` whose accessors are gated to the field's category, so reading a
`String` field as an integer is a compile error, not a runtime surprise — full
type safety at zero runtime cost.

Wire format only: no session layer, no transport. On top of upstream it adds
indexed and repeating-group reads, drops Boost (`chrono` + epoch ints), and
ships a Google Benchmark suite. Porting guide: [API_CHANGES.md](API_CHANGES.md).

## Quickstart

```cpp
#include <nanofix.hpp>

// read — tag::Price is a typed handle; find() returns a typed_value whose
// accessors are restricted to the field's FIX type (wrong one = compile error).
nanofix::message_reader r(buf, buf + n);
if (r.is_complete() && r.is_valid()) {
    long mant, exp;
    if (r.find(nanofix::tag::Price).try_as_decimal(mant, exp))  // mant * 10^exp
        use(mant, exp);
    // r.find(nanofix::tag::Symbol).try_as_int(x);   // would not compile (String)

    for (auto it = r.begin(); it != r.end(); ++it)   // or stream every field
        dispatch(it->tag(), it->value());            // raw field_value, any type
}

// write
char out[256];
nanofix::message_writer w(out);
w.push_back_header("FIXT.1.1");
w.push_back_string(nanofix::tag::MsgType, "D");
w.push_back_int(nanofix::tag::OrderQty, 100);
w.push_back_decimal(nanofix::tag::Price, 50001, -2);
if (!w.push_back_trailer()) return too_small();          // false == didn't fit
char* end = w.message_end();
```

Many messages in one buffer: `nanofix::for_each_message(begin, end, fn)` hands
back each complete, valid reader and resyncs past garbage.

## Choosing an access path

Same wire, same reader; these differ only in how you locate a tag, and you can
mix them. Default to the iterator; reach for the rest when the lookup count or a
stable venue justifies it. Each also works on one repeating-group entry.

| Path | Use when | How |
| --- | --- | --- |
| **Iterator** | the default — a few lookups, or fields read once in wire order | `r.find(tag::X)`, `r.find_with_hint(tag::X, it)`, or `for (it = r.begin(); ...)` |
| **`nanofix::with_fields(r, buf, fn)`** | many lookups, want an index with automatic iterator fallback | `with_fields(r, buf, [&](auto& f){ f.find(tag::X); })` |
| **Indexed** (`build_field_index`) | many lookups (~6–8+) into one message | `build_field_index(r, buf)` once, then `find(tag::X)` |

Every `tag::X` is a **typed handle**. `find(tag::X)` / `find_with_hint(tag::X,
it)` return a `typed_value` exposing only the accessors valid for that field's
FIX type — `try_as_decimal` for `Price`, `as_string_view` for `Symbol`; the
wrong one is a compile error. `bytes()` / `as_string_view()` /
`as_char_unchecked()` stay available on any typed value, and `.value()` drops to
the raw `field_value` (ungated `try_*` / `as_*_unchecked`, the escape hatch for
off-spec data). The `tag::` namespace needs no extra header and costs nothing at
runtime when the tag is known at compile time. `tag::X` also converts to its
`int` number, so writer / comparison / `group()` call sites take it unchanged.

`find(tag::X)` scans from the start; `find_with_hint(tag::X, it)` carries a
cursor, so reading tags in wire order stays cheap. An absent tag scans to
end-of-message — don't loop `find` over many optional tags (O(N × length));
index instead.

### Tiered `with_fields(r, buf, fn)` — index with iterator fallback

`with_fields` builds the index, checks `truncated()` **once**, then calls your
callback with a concrete accessor: `indexed_fields<N>` when the message fit the
buffer, `iter_fields` when it exceeded `N` fields. Each accessor's `find(tag)`
is branch-free — the index↔iterator decision lives in the dispatch, not the
per-lookup hot path. Write the callback as a generic lambda; it is instantiated
for both accessor types. The buffer is yours, reused per message:

```cpp
nanofix::field_index_buffer<64> buf;
for (auto const& m : nanofix::messages(wire)) {
    nanofix::with_fields(m, buf, [&](auto& f) {
        long qty_m = 0, qty_e = 0;
        f.find(tag::OrderQty).try_as_decimal(qty_m, qty_e);  // OrderQty is a Qty
        auto px = f.find(tag::Price);          // empty() if absent
    });
}
```

`build_field_index(r, buf)` is the same index without the dispatch wrapper; on
`truncated()` (message exceeded `N` fields) it returns an empty index, so
branch back to the iterator yourself. When you know a message fits, wrap that
index directly: `nanofix::indexed_fields<N> f(build_field_index(r, buf));`.

### Repeating groups

A whole-message index can't address group fields (repeating tags collide — a
`find` returns only the first entry's), so a group is read per entry. Feed the
group's count tag and its delimiter to the runtime `group(count, delim)`
overload, then read each entry with the per-entry iterator. The delimiter is
per-MsgType: a `NoMDEntries` in a snapshot starts at `MDEntryType`, in an
incremental at `MDUpdateAction`.

```cpp
m.group(tag::NoMDEntries, tag::MDUpdateAction)  // incremental delimiter
 .for_each([&](nanofix::group_entry const& e) {
     auto it = e.begin();
     long mant, exp;
     if (e.find_with_hint(tag::MDEntryPx, it).try_as_decimal(mant, exp)) {  // typed
         use(mant, exp);
     }
 });
```

`build_field_index(entry, buf)` also works on a single entry when an entry
carries enough fields to amortize the index. See
[examples/fix50_mdmonitor](examples/fix50_mdmonitor/) for a worked group reader.

## Build

CMake 3.20+, Conan 2.x, C++20 toolchain.

```sh
pip install 'conan>=2.0,<3.0'
conan profile detect --force

conan install . --output-folder=build --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20
cmake --preset conan-release
cmake --build build/build/Release -j
ctest --test-dir build/build/Release
```

`conan install` lays the build tree out under `build/build/Release` (the
`conan-release` preset points there). A local checkout is the top-level CMake
project, so `NANOFIX_BUILD` defaults ON and the tests, benchmarks, and CLI
build without any extra flag — the `conanfile` forces them off only when
nanofix is packaged as a dependency (`conan create`). The CLI binaries land in
`build/build/Release/utils/` (`fixgen`, `fixprint`, `fixspec-gen`).

### As a Conan 2 package

`conanfile.py` declares `nanofix/1.0.0`, header-only, CMake target
`nanofix::nanofix`:

```sh
conan create . --build=missing \
    -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20 \
    -c tools.build:skip_test=True
```

```cmake
find_package(nanofix REQUIRED)
target_link_libraries(my_app PRIVATE nanofix::nanofix)
```

### Example projects

[`examples/`](examples/) holds standalone Conan 2 projects that consume
`nanofix` as a downstream user would (publish to the local cache first, then
build each on its own; exercised in CI):

| App | Spec | Shows |
| --- | --- | --- |
| [fix44_gateway](examples/fix44_gateway/) | QuickFIX `FIX44.xml` | `message_writer`, `for_each_message`, `with_fields` (index↔iterator), custom-spec codegen via `nanofix_generate()` |
| [fix50_mdmonitor](examples/fix50_mdmonitor/) | bundled FIX 5.0 SP2 + FIXT 1.1 | per-MsgType `NoMDEntries` repeating-group read on market-data snapshot/incremental (delimiter differs per MsgType), per-entry typed `find(tag::X)`, enum decode via `nanofix/names.hpp` |

## Benchmarks

`benchmarks/` is a deep Google Benchmark suite (upstream ships none): every
access path, warm and cold, sequential and random, plus write/read tail
latency, SIMD primitives, and index amortization curves.

`compare2upstream/run.sh` is the differential harness. It builds upstream, this
fork, and a scalar (`NANOFIX_DISABLE_SIMD`) config, runs **only the benches the
overview table needs** (`--benchmark_filter`), and `render.py` prints a small
README-shaped overview — four compact tables, not the full suite:

- **Write/read latency** (iterator path): write p99/p999, read p99/p999
  (sequential and random).
- **Write/read throughput** (iterator path): write,
  read sequential, read random.
- **New access path vs the upstream iterator — latency**: `build_field_index`,
  p99/p999 vs upstream's only read path, shown SIMD-on and SIMD-off.
- **New access path vs the upstream iterator — throughput**: the same path and
  workload (SIMD-on and SIMD-off), message rate.

### Indicative numbers

> **macOS, Apple M4, unpinned — indicative only.** macOS has no hard core
> pinning and Apple Silicon migrates work across P/E cores mid-run, so these
> drift run-to-run. Read them as rough shape, not measurements. `nanofix` is
> `fork`; `fork-no-simd` is the same code with `NANOFIX_DISABLE_SIMD`; upstream
> is `jamesdbrock/hffix`. Generated by `compare2upstream/run.sh`
> (`min_time=0.3s`, `repetitions=5`).

Write/read latency — lower is better; upstream is the baseline. Iterator path
(`find_with_hint` over a fixed **20-tag** set per message); tail percentiles
include the ~20-30 ns `clock::now()` probe. The indexed path cuts the read tail
by an order of magnitude — see the new-access-path latency table below:

| Benchmark | upstream | fork | fork-no-simd |
| --- | --- | --- | --- |
| `Write p99` | 116.8 ns | **84 ns (-28 %)** | 84 ns (-28 %) |
| `Write p999` | 125 ns | **84 ns (-33 %)** | 84 ns (-33 %) |
| `Read seq p99` | 82317 ns | 59641 ns (-28 %) | **58917 ns (-28 %)** |
| `Read seq p999` | 92333 ns | 62817 ns (-32 %) | **61942 ns (-33 %)** |
| `Read rand p99` | 128300 ns | 87575 ns (-32 %) | **87367 ns (-32 %)** |
| `Read rand p999` | 154450 ns | 93359 ns (-40 %) | **91858 ns (-41 %)** |

Write/read throughput — higher is better; iterator path, upstream
is the baseline:

| Benchmark | upstream | fork | fork-no-simd |
| --- | --- | --- | --- |
| `Write (NewOrder)` | 13.2 M msgs/s | **19.8 M msgs/s (+51 %)** | 19.8 M msgs/s (+50 %) |
| `Read seq` | 31.7 k msgs/s | **44.5 k msgs/s (+40 %)** | 44.3 k msgs/s (+40 %) |
| `Read random` | 21.7 k msgs/s | 27.9 k msgs/s (+29 %) | **30 k msgs/s (+38 %)** |

New access path vs the upstream iterator — latency (ns), lower is better. Same
20-tag workload as the throughput table below; reading many tags makes the
iterator rescan per lookup, while `build_field_index` is O(length) regardless
of lookup count. Shown SIMD-on (`fork`) and SIMD-off (`fork-no-simd`) — SIMD
`find_all_soh` drives the indexed path's bulk framing, so turning it off roughly
triples its tail.

| Benchmark | upstream iterator | fork indexed | fork-no-simd indexed |
| --- | --- | --- | --- |
| `Read seq p99` | 82317 ns | **4741 ns (-94 %)** | 13434 ns (-84 %) |
| `Read seq p999` | 92333 ns | **6475 ns (-93 %)** | 23408 ns (-75 %) |
| `Read rand p99` | 128300 ns | **4975 ns (-96 %)** | 13392 ns (-90 %) |
| `Read rand p999` | 154450 ns | **5350 ns (-97 %)** | 14242 ns (-91 %) |

New access path vs the upstream iterator — message throughput, higher is
better, delta vs upstream's iterator on the same workload. Shown SIMD-on
(`fork`) and SIMD-off (`fork-no-simd`):

| Benchmark | upstream iterator | fork indexed | fork-no-simd indexed |
| --- | --- | --- | --- |
| `Read seq` | 31.7 k msgs/s | **535 k msgs/s (+1586 %)** | 216 k msgs/s (+582 %) |
| `Read random` | 21.7 k msgs/s | **502 k msgs/s (+2214 %)** | 193 k msgs/s (+787 %) |

## Generated spec headers

`include/nanofix/detail/fields.hpp` (typed `tag::` handles) and `names.hpp` (opt-in
human-readable names) carry the spec data. They are committed; regenerate from
QuickFIX-format XML after a spec change — one invocation emits both into the
output dir under fixed names:

```sh
build/build/Release/utils/fixspec-gen fixspec/FIX50SP2.xml fixspec/FIXT11.xml \
    -d include/nanofix
```

FIX 5.0 needs both files (session FIXT.1.1 + application FIX.5.0 SP2); append
venue XMLs as further positional args. Downstream targets can regenerate at
build time:

```cmake
nanofix_generate(TARGET my_app
    SPEC_XML my_spec/FIX50SP2.xml my_spec/FIXT11.xml)
```

## Build options

| Option | Default | Effect |
| --- | --- | --- |
| `NANOFIX_BUILD` | ON top-level / OFF as subdir | Tests, benchmarks, CLI. |
| `NANOFIX_NATIVE_ARCH` | ON | `-mcpu=native` / `-march=native`. |
| `NANOFIX_LTO` | ON | IPO (Clang/AppleClang forced to `-flto=full`). |
| `NANOFIX_DISABLE_SIMD` | OFF | Force scalar `find_all_soh` / `find_tag_in_index` / `checksum_bytes`. Required for pre-Haswell x86 (the SIMD build uses AVX2 unconditionally). |
| `NANOFIX_SANITIZE` | OFF | ASan + UBSan (use with Debug, `NANOFIX_LTO=OFF`). |
| `NANOFIX_SANITIZE_THREAD` | OFF | TSan. Mutually exclusive with `NANOFIX_SANITIZE`. |
| `NANOFIX_BENCH_MESSAGES` | 500000 | Synthetic dataset size for `bench_data`. |

```sh
# debug + sanitizers
conan install . --output-folder=build-asan --build=missing \
    -s build_type=Debug -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20
cmake --preset conan-debug -DNANOFIX_SANITIZE=ON -DNANOFIX_LTO=OFF -DNANOFIX_NATIVE_ARCH=OFF
cmake --build build-asan/build/Debug -j && ctest --test-dir build-asan/build/Debug
```

## SIMD dispatch

`find_all_soh`, `find_tag_in_index`, `checksum_bytes` ship scalar / NEON / AVX2
impls, selected at compile time by `#if/#elif/#else` on `NANOFIX_HAS_NEON` /
`NANOFIX_HAS_AVX2` — no runtime CPUID, no per-call overhead. NEON is the aarch64
baseline (mandatory since ARMv8); AVX2 is the x86-64 baseline (Haswell, 2013),
so **an x86-64 binary will not run on pre-Haswell CPUs** unless built with
`NANOFIX_DISABLE_SIMD=ON`. SIMD wins on bulk, whole-message work: the framing
sweep (`find_all_soh`) behind `build_field_index`, indexed tag lookup
(`find_tag_in_index`), and the checksum of large messages. The plain
**iterator path uses no SIMD** — per-field SOH search is `find_soh` (a
short serial scan loses to branch-predicted byte compares) and the checksum is
computed only on demand. So `NANOFIX_DISABLE_SIMD` changes the indexed path,
not the iterator; on the iterator path SIMD-on vs SIMD-off differ only by binary
layout, which on a cache-miss-bound random read is run-to-run noise (the
macOS-unpinned caveat above the benchmark tables applies — the sign of that
delta flips between runs). Supported: GCC, Clang, AppleClang, MSVC (x86-64 and
ARM64).

## Fuzzing

A standalone CMake project under [fuzz/](fuzz/) builds a libFuzzer + ASan +
UBSan binary over `message_reader`, `for_each_message`, repeating groups,
`build_field_index`, and the `try_as_*` family, seeded from
[tests/data/](tests/data/) and a FIX-token dictionary
([fuzz/fix.dict](fuzz/fix.dict)). Replay a crash with
`build/fuzz/fuzz_reader ./crash-<hash>`.

## Thread safety and errors

Reader and writer methods are `noexcept`; errors surface via return code or a
sticky flag (`is_valid()` / `ok()`), never an exception. Programmer-error
invariants use `NANOFIX_ASSERT` (an atomic counter + optional handler, no
`abort` unless built with `-DNANOFIX_ASSERT_FAILFAST`). The buffer is
caller-owned and never mutated by a reader, so any number of `message_reader`s
over the same `const` buffer are safe to use concurrently across threads
(TSan-validated; see `tests/threading_tests.cpp`).
