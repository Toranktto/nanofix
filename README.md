# nanofix

[![CI](https://github.com/Toranktto/nanofix/actions/workflows/ci.yml/badge.svg)](https://github.com/Toranktto/nanofix/actions/workflows/ci.yml)
[![License: BSD-2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-informational.svg)](#build)

A header-only C++20 FIX 4.x/5.0 parser built for low-latency systems, forked
from [jamesdbrock/hffix](https://github.com/jamesdbrock/hffix). Every read and
write path allocates nothing, throws nothing, copies nothing — you own the
buffer, the reader views it, the writer fills it. SIMD (AVX2/NEON) drives the
hot scan, checksum, and indexed tag lookup; the tail percentiles are in the
[benchmark tables](#benchmarks) below.

Tags carry their FIX type at compile time. `find(tag::Price)` returns a
`typed_value` whose accessors are gated to the field's category, so reading a
`String` field as an integer is a compile error, not a runtime surprise.

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
w.push_back_decimal(nanofix::tag::OrderQty, 100, 0);  // Qty is decimal
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
wrong one is a compile error. `bytes()` / `as_string_view()` stay available on
any typed value, and `.value()` drops to
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

Sharp edge: entry boundaries are "next delimiter occurrence", so the *last*
entry extends to the end of the body — message-level fields placed after the
group (Text, venue tails) land inside it and entry-level `find()` will match
them. Read post-group fields at message level, and pick the delimiter from a
per-(MsgType, group) table rather than inlining it at call sites: a wrong
delimiter yields wrong entry boundaries silently, not an error.

### Streaming ingest and rejects

A socket read rarely ends on a message boundary. `for_each_message` yields
each complete, valid frame, silently resyncing past garbage, and returns the
start of the unconsumed tail — copy that tail to the buffer head and read on:

```cpp
char ring[1 << 16];
std::size_t used = 0;  // bytes carried over from the previous recv
for (;;) {
    auto n = recv(fd, ring + used, sizeof(ring) - used, 0);
    if (n <= 0)
        break;
    std::span<char const> wire(ring, used + static_cast<std::size_t>(n));

    char const* tail = nanofix::for_each_message(wire, [&](nanofix::message_reader const& r) {
        unsigned char wire_sum = 0;  // checksum policy: drop on mismatch
        if (!r.check_sum()->value().try_as_int(wire_sum) || wire_sum != r.calculate_check_sum())
            return;
        dispatch(r);
    });

    used = static_cast<std::size_t>(wire.data() + wire.size() - tail);
    std::memmove(ring, tail, used);  // incomplete frame -> buffer head
}
```

A session gateway that must *reject* mis-framed input instead of skipping it
walks readers explicitly — `for_each_message` never surfaces invalid frames:

```cpp
for (nanofix::message_reader r(wire); r.is_complete(); r = r.next_message_reader()) {
    if (!r.is_valid()) {
        emit_session_reject(r.error());  // parse_error mirrors SessionRejectReason
        continue;                        // next_message_reader() resyncs on "8=FIX"
    }
    dispatch(r);
}
```

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

`conanfile.py` derives the package version from the latest `v*` git tag
(`0.0.0+g<sha>` on an untagged checkout); header-only, CMake target
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

### Plain CMake (no Conan)

`add_subdirectory(nanofix)` exposes `nanofix::nanofix` directly. Or install a
relocatable package — the library itself has zero dependencies:

```sh
cmake -S . -B build-install -DNANOFIX_BUILD=OFF -DNANOFIX_BUILD_FIXSPEC_GEN=OFF
cmake --install build-install --prefix /opt/nanofix
```

```cmake
find_package(nanofix CONFIG REQUIRED)   # CMAKE_PREFIX_PATH=/opt/nanofix
target_link_libraries(my_app PRIVATE nanofix::nanofix)
```

The installed config also provides `nanofix_generate()`; build with
`-DNANOFIX_BUILD_FIXSPEC_GEN=ON` (needs pugixml) to install the `fixspec-gen`
binary it drives.

### Example projects

[`examples/`](examples/) holds standalone Conan 2 projects that consume
`nanofix` as a downstream user would (publish to the local cache first, then
build each on its own; exercised in CI):

| App | Spec | Shows |
| --- | --- | --- |
| [fix44_gateway](examples/fix44_gateway/) | QuickFIX `FIX44.xml` | `message_writer`, `for_each_message`, `with_fields` (index↔iterator), custom-spec codegen via `nanofix_generate()` |
| [fix50_mdmonitor](examples/fix50_mdmonitor/) | bundled FIX 5.0 SP2 + FIXT 1.1 | per-MsgType `NoMDEntries` repeating-group read on market-data snapshot/incremental (delimiter differs per MsgType), per-entry typed `find(tag::X)`, tiered per-entry index (`build_field_index(entry, …)` with forward-scan fallback), enum decode via `nanofix/names.hpp` |

## Benchmarks

`benchmarks/` is a deep Google Benchmark suite (upstream ships none): every
access path, warm and cold, sequential and random, plus write/read tail
latency, SIMD primitives, and index amortization curves.

`compare2upstream/run.sh` is the differential harness. It builds upstream, this
fork, and a scalar (`NANOFIX_DISABLE_SIMD`) config, runs **only the benches the
overview tables need** (`--benchmark_filter`), and `render.py` renders a small
README-shaped overview — five compact tables, not the full suite:

- **Write/read latency** (iterator path): write p99/p999, read p99/p999
  (sequential and random).
- **Write/read throughput** (iterator path): write,
  read sequential, read random.
- **New access path vs the upstream iterator — latency**: `build_field_index`,
  p99/p999 vs upstream's only read path, shown SIMD-on and SIMD-off.
- **New access path vs the upstream iterator — throughput**: the same path and
  workload (SIMD-on and SIMD-off), message rate.
- **Index amortization**: the break-even `find()` count per message at which
  `build_field_index` + indexed lookups overtake the iterator.

### Indicative numbers

`compare2upstream/run.sh` prints the rendered tables to stdout (progress goes
to stderr); the
committed snapshot is captured by redirecting, e.g.
`compare2upstream/run.sh > X86_64.md`:

- **[X86_64.md](X86_64.md)** — Intel Cascade Lake (GCE `c2-standard-4`, SMT
  off), Linux, pinned to a core isolated via `isolcpus`/`nohz_full`; generated
  by `scripts/gcloud_compare2upstream.py`. Still a VM — no governor/turbo control
  from the guest — so treat it as one notch below a bare-metal `isolcpus` box.

Headline shape on x86-64 (Cascade Lake): fork writes with ~68 % lower tail
(p99 77 ns vs 239 ns) and ~12 % more throughput than upstream; iterator-path
reads are ~20 % faster than upstream; the indexed path is ~10-14× upstream's
read throughput and cuts the read tail by an order of magnitude (p99 8.6 µs
vs 85 µs), paying for itself from ~7 `find()`s per message.

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
layout, which on a cache-miss-bound random read is run-to-run noise — the sign
of that delta flips between runs. Supported: GCC, Clang, AppleClang, MSVC
(x86-64 and ARM64).

## Fuzzing

A standalone CMake project under [fuzz/](fuzz/) builds a libFuzzer + ASan +
UBSan binary over `message_reader`, `for_each_message`, repeating groups,
`build_field_index`, the typed `find(tag::X)` facade, and the full `try_as_*`
family including the validating chrono tier, seeded from
[tests/data/](tests/data/) and a FIX-token dictionary
([fuzz/fix.dict](fuzz/fix.dict)). CI runs a 60 s smoke per push and a 1 h
nightly campaign (`fuzz-nightly.yml`), both feeding one rolling corpus cache.
Replay a crash with `build/fuzz/fuzz_reader ./crash-<hash>`.

## Thread safety and errors

Reader and writer methods are `noexcept`; errors surface via return code or a
sticky flag (`is_valid()` / `ok()`), never an exception. Programmer-error
invariants use `NANOFIX_ASSERT` (an atomic counter + optional handler, no
`abort` unless built with `-DNANOFIX_ASSERT_FAILFAST`). The buffer is
caller-owned and never mutated by a reader, so any number of `message_reader`s
over the same `const` buffer are safe to use concurrently across threads
(TSan-validated; see `tests/threading_tests.cpp`).

Sharing is per *buffer*, not per accessor object. Anything with an internal
cursor or index is one-thread-at-a-time by design: `indexed_fields<N>` mutates
its shared lookup hint on every `find()` (share the underlying
`indexed_message` and call `find_with_hint` with a per-thread hint instead),
`iter_fields` advances a cursor, a `field_index_buffer` must not be rebuilt
while another thread reads an `indexed_message` over it, and `message_writer`
is one thread, one buffer. Rule of thumb: share `const` bytes freely; give
every thread its own accessor objects.

## Production readiness

Every push runs the full gate: build + tests on Linux (GCC, Clang), macOS
(AppleClang) and Windows (MSVC, clang-cl); ASan+UBSan and TSan suites; a 60 s
libFuzzer smoke over the reader (dictionary-driven, rolling corpus cache); a
scalar (`NANOFIX_DISABLE_SIMD`) build and test run; clang-format and
clang-tidy (warnings are errors); a warning-free Doxygen build; and both
example projects built as downstream Conan consumers. The test suite covers malformed-input and
truncation fixtures, a characterization diff holding the SIMD bulk framing
byte-identical to a per-field reference parser, and concurrent-reader
threading tests.

Deployment notes, roughly in priority order:

- **Wire an assert handler.** `nanofix::set_assert_handler` sees every
  `NANOFIX_ASSERT` failure (caught programmer errors, e.g. iterating an
  invalid message). The default build only bumps
  `nanofix::assert_failure_count()` and continues — alert on that counter.
  Build test/staging with `-DNANOFIX_ASSERT_FAILFAST` so invariant bugs abort
  loudly before they ship.
- **Decide a checksum policy.** `is_valid()` validates framing structure, not
  the CheckSum value. If the transport or compliance requires it, verify at
  ingress with `calculate_check_sum()` — `examples/fix44_gateway` shows the
  gate.
- **Default to `try_as_*` off the wire.** The `*_unchecked` readers are for
  externally validated fields; on garbage their output is undefined
  (documented per method). The parts-based `as_*` time accessors
  (`as_timeonly(h,m,s,ms)`, `as_date(y,m,d)`, …) validate length only —
  range-check after parsing. Everything that produces an epoch or
  `time_point` (`as_epoch_millis` / `as_epoch_nanos`, `as_timestamp(tp)`,
  `try_as<std::chrono::...>`) is fully validating: digits, separators,
  calendar (incl. day-in-month) and clock ranges.
- **Size `field_index_buffer<N>` for the venue.** Overflow is all-or-nothing:
  `truncated()` yields an empty index and `with_fields` falls back to the
  iterator — a tail-latency cliff, not an error. Monitor `truncated()` in
  production.
- **Check the CPU baseline.** x86-64 builds assume AVX2 (Haswell, 2013+);
  ship `-DNANOFIX_DISABLE_SIMD=ON` for older fleets. `NANOFIX_NATIVE_ARCH`
  (default ON) tunes for the build host — turn it off for binaries that move
  between machines (the Conan recipe already builds its packaged `fixspec-gen`
  with it OFF so binary packages stay portable).
- **Pin a release.** `NANOFIX_VERSION` / `NANOFIX_VERSION_{MAJOR,MINOR,PATCH}`
  (via `<nanofix.hpp>`) identify the header set at compile time. Version truth
  is git: the latest `v*` tag (`git describe`), with dev builds stamped
  `X.Y.Z+<n>.g<sha>`. `nanofix/detail/version.hpp` is generated into the
  build tree at CMake configure and installed with the package; the committed
  header is a `0.0.0+unknown` stub for builds that bypass CMake.
- **Benchmark on your hardware.** The linked tables come from a cloud VM
  (pinned, `isolcpus`-isolated, but no governor/turbo control). Before acting
  on a number, reproduce it on the hardware you deploy to:
  `NANOFIX_BENCH_CPU=<core> compare2upstream/run.sh`.

Out of scope by design: session state, sequence numbers, retransmission,
persistence, transport. nanofix is the codec under your engine, not the
engine.

## License

BSD-2-Clause — see [LICENSE](LICENSE). Fork of
[jamesdbrock/hffix](https://github.com/jamesdbrock/hffix); upstream copyright
retained alongside fork modifications.
