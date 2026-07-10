# Porting from upstream hffix

For moving code off `jamesdbrock/hffix`.

The wire format and the day-to-day reader/writer loop are unchanged. Three
things break a straight drop-in; everything else is additive.

## The three breaking changes

**Exceptions are gone.** Every reader and writer method is `noexcept`.

Upstream threw from the writer on a full buffer or on misuse, and from
`as_string()` on allocation failure. Here the writer keeps a sticky error
flag: `push_back_*` return `void`, the first overflow trips the flag, and
later calls no-op. Write the whole message, check once:

```cpp
nanofix::message_writer w(buf);
w.push_back_header("FIX.4.4");
w.push_back_string(nanofix::tag::MsgType, "D");
// ...
if (!w.push_back_trailer())     // false == didn't fit
    return too_small();
```

`ok()` checks mid-stream if you need it. A malformed *read* buffer still
surfaces through `is_valid()` / `is_complete()` as before — but calling an
accessor on a reader you never validated is now a caught programmer error
(`NANOFIX_ASSERT`) instead of UB.

**`std::string` is gone.** `as_string()` is removed; read with
`as_string_view()` or `bytes()` (a `std::span<char const>`, may hold embedded
SOH/NUL) and copy yourself if you need ownership. `push_back_string` takes
`(begin, end)`, a `string_view`, or a literal — wrap a runtime `char*` in a
`string_view` at the call site so `strlen` stays off the writer path.

**Boost time is gone.** No `boost::gregorian` / `boost::posix_time`. The
accessors stayed, now `noexcept` and writing through out-params:
`as_date(y, m, d)`, `as_timeonly[_nano]`, `as_timestamp[_nano]`. New on top:
`as_epoch_nanos()` / `as_epoch_millis()` (return `std::optional<int64_t>`),
`chrono` out-param overloads, and the matching writer overloads
(`push_back_timestamp_epoch_millis` / `_nanos`). Epoch-materializing paths
reject `year` outside `[1970, 2200]` and non-digit fractional bytes so a
malformed timestamp can't overflow the `int64_t` arithmetic.

## Reading a field: pick checked or unchecked

Upstream's `as_int` / `as_decimal` never validated. That code is still here,
renamed to say so, with checked siblings:

| upstream | here |
| --- | --- |
| `as_int<T>()` | `as_int_unchecked<T>()` |
| `as_decimal(m, e)` | `as_decimal_unchecked(m, e)` |
| — | `try_as_char(char&) -> bool` |
| — | `try_as_int<T>(T&) -> bool` |
| — | `try_as_decimal<T>(T&, T&) -> bool` |
| — | `try_as_bool(bool&) -> bool` |
| — | `as_bool_unchecked() -> bool` |
| — | `try_as<T>(T&) -> bool` |
| — | `as_unchecked<T>() -> T` |

Default to `try_*` for anything off the wire: it validates digits and integer
overflow, and is `noexcept`. Keep `_unchecked` only for fields you already
validated — the suffix makes the call site admit it.

`field_value::try_as<T>(T&)` / `as_unchecked<T>()` are type-dispatched facades
over the named methods above. `T` is an integral, `bool`, `char`,
`std::string_view`, or `decimal_parts<Int>` — a plain `{mantissa, exponent}`
carrier (`Int` defaults to `int64_t`) that routes to `try_as_decimal` /
`as_decimal_unchecked`. `bool` is matched before the integral branch, so
`try_as<bool>` decodes FIX Boolean (`'Y'`/`'N'`), not `0`/`1`.

For wire data prefer the validating reads: `try_as_char` rejects any length
other than one (a FIX `char` is one byte), `try_as_bool` accepts only
`'Y'`/`'N'`. The `as_*_unchecked` forms read blind — UB on empty, silent
truncation on a multi-byte `char`. On `typed_value`, `try_as_char` /
`try_as_bool` are gated to the `character` category.

```cpp
std::uint32_t seq = 0;
f.find(tag::MsgSeqNum).value().try_as(seq);           // -> try_as_int
nanofix::decimal_parts<> px{};
f.find(tag::Price).value().try_as(px);                // -> try_as_decimal, px.mantissa / px.exponent
auto qty = f.find(tag::OrderQty).value().as_unchecked<std::int64_t>();
```

## Choosing an access path

No upstream equivalent. Three ways to read a message; they differ only in how you
locate fields, and you can mix them. Each also works on one repeating-group
entry (see [Repeating groups](#repeating-groups)).

| Path | Reach for it when | Cost |
| --- | --- | --- |
| **Iterator** (`begin()`/`find_with_hint`) | the default; few lookups, or fields read once in wire order | one forward scan per lookup |
| **Indexed** (`build_field_index`) | many lookups into the same message (~6–8+) | one scan to build, then a SIMD-linear lookup |
| **`nanofix::with_fields(...)`** | index with an automatic iterator fallback | index when it fits, iterator when `truncated()`, decided once |

Rules of thumb:

- Reading a handful of tags, or streaming once — stay on the **iterator**.
  `find_with_hint` carries a cursor, so reading tags in wire order is cheap;
  an absent tag scans to end-of-message, so don't loop `find` over many
  optional tags (that is O(N × length) — use a path below instead).
- Pulling 8+ fields out of one message — **`build_field_index`** once, then
  look up. `N` is caller-fixed; an overrun sets `truncated()` and drops the
  index to zero usable fields (fall back to the iterator for that message),
  never reallocates.
- Don't want to branch on `truncated()` — **`nanofix::with_fields(reader, buf,
  fn)`**. It builds the index, checks `truncated()` once, then calls `fn` with a
  concrete `indexed_fields<N>` (message fit the buffer) or `iter_fields` (it did
  not); each accessor's `find(tag)` is branch-free. Write `fn` as a generic
  lambda — it is instantiated for both accessor types.

### Examples

`with_fields(r, buf, fn)` — the buffer is yours, reused per message:

```cpp
nanofix::field_index_buffer<64> buf;
for (auto const& m : nanofix::messages(wire)) {
    nanofix::with_fields(m, buf, [&](auto& f) {
        int qty = 0;
        f.find(tag::OrderQty).try_as_int(qty);
        auto px = f.find(tag::Price);      // empty() if absent
    });
}
```

Index/iterator on a group entry: `build_field_index(entry, buf)` for many
fields, `find_with_hint` for a few. `with_fields()`'s message-level
index↔iterator dispatch has no entry overload, so branch on `truncated()`
yourself (`group_entry::find_with_hint(tag, it)` returns a bool and advances
`it`):

```cpp
nanofix::field_index_buffer<32> ibuf;
m.group(tag::NoMDEntries, tag::MDEntryType).for_each([&](nanofix::group_entry const& e) {
    auto idx = nanofix::build_field_index(e, ibuf);
    nanofix::field_value px;
    if (!idx.truncated()) {
        std::size_t h = 0;
        px = idx.find_with_hint(tag::MDEntryPx, h);
    } else {                                  // entry overran ibuf -> iterate
        auto it = e.begin();
        if (e.find_with_hint(tag::MDEntryPx, it)) px = it->value();
    }
});
```

## Repeating groups

`reader.group(count_tag, delim_tag)` returns a `group_view` you walk with
`for_each`, yielding a `group_entry` per entry. There is no compile-time
`group<CountTag>()`: a group's delimiter (the first tag of each entry) can differ
per MsgType — `NoMDEntries` starts at `MDEntryType` in a snapshot but
`MDUpdateAction` in an incremental — so the delimiter is always passed
explicitly. Nest with `entry.group(...)`. An entry is a field range, so it takes
the same paths as a message: `find_with_hint` for a few fields,
`build_field_index(entry, ...)` for many. A whole-message index can't reach
group fields — repeating tags collide — so a group is always read per entry.

## Batched parse

`nanofix::messages(buf)` (a range) and `nanofix::for_each_message(begin, end, fn)`
walk many messages in one buffer. They hand back only complete, valid readers,
resync past invalid frames, and expose the unconsumed tail so a partial
trailing message can be re-fed on the next read.

## Smaller changes

- **`is_known_tag(int)`** — `constexpr` bitmap of every tag in the loaded spec
  (`nanofix/detail/fields.hpp`). The parser does not range-check tag numbers; this is
  the filter for the iteration loop.
- **Names** — `field_name(tag)`, `msg_type_name(value)`, `value_name(tag,
  value)` in the opt-in `nanofix/names.hpp` (not pulled in by `nanofix.hpp`), each
  `constexpr` returning a `string_view` into static storage. No `<string>`,
  `<map>`, or `<iostream>`. Include it only where you log or tool. This
  replaces upstream's stream-only `field_name`.
- **No `<iostream>`** — the operator`<<` overloads are gone (they injected
  `std::ios_base::Init` into every TU). Print a value's bytes directly:
  `std::printf("%.*s", int(v.size()), v.begin())`.
- **Iterator category** — `message_reader_const_iterator` now advertises
  `forward_iterator_tag` with `const`-correct typedefs, so standard algorithms
  take their multipass paths. No call-site change.
- **`push_back_trailer`** — the checksum toggle is a template parameter
  (`push_back_trailer<false>()` to skip), not a runtime `bool`.
- **`find_with_hint`** is also a reader member, `reader.find_with_hint(tag,
  it)`, beside the upstream free function.
- **Asserts** — `assert_failure_count()` / `reset_assert_failure_count()` read
  and clear the counter, `set_assert_handler()` installs a callback,
  `-DNANOFIX_ASSERT_FAILFAST` traps. `NANOFIX_ASSERT` is defined unconditionally;
  customize via `set_assert_handler`, not by redefining the macro.
- **Version macros** — `NANOFIX_VERSION` (string, e.g. `1.2.3` or
  `1.2.3+5.gabc1234` on dev builds) and `NANOFIX_VERSION_MAJOR` / `_MINOR` /
  `_PATCH` (ints) in `nanofix/detail/version.hpp`, available through
  `<nanofix.hpp>`. Version truth is the latest `v*` git tag (`git describe`);
  CMake configure generates the real header into the build tree (installed
  with the package), the committed header is a `0.0.0+unknown` stub for
  non-CMake use, and CI cross-checks the CMake and Python derivations.
  Upstream had no compile-time version identification.
- Class names you already use are unchanged (`message_reader`,
  `message_writer`, `field_value`, `field`). Added: `indexed_message`,
  `group_view`, `group_entry`, `message_range`, the caller-sized buffer
  `field_index_buffer<N>`, and the typed-tag types `field_tag`, `typed_value`,
  `fix_type`.
- **Typed tags** — every `tag::<Name>` is now a `field_tag<Tag, fix_type>`
  carrying the field's FIX value-type category (it was a plain enum int
  upstream; it converts to its `int` number via `operator int()`, so
  `push_back_*(tag::X, …)`, `it->tag() == tag::X`, `group(tag::X, …)` are
  unchanged). `find(tag::Price)` and `find_with_hint(tag::Price, cur)` — on
  `message_reader`, `group_entry`, `indexed_message`, `indexed_fields`,
  `iter_fields` — return a `typed_value<Type>` whose `try_as_int` /
  `try_as_decimal` / `try_as_char` / `try_as_bool` / time accessors compile only
  for the matching category;
  `bytes()` / `as_string_view()` / `as_char_unchecked()` / `empty()` / `operator bool`
  stay universal, and `.value()` returns the raw `field_value` for the ungated /
  `_unchecked` escape hatch. Wrong-type access (`try_as_int` on a `String`
  field) is a compile error. No runtime cost when the tag is known at compile
  time. The runtime-`int` overloads (`find_with_hint(int, …)`) stay for dynamic
  tags. `field_value` (from `it->value()`) keeps its full ungated API.
- **`dictionary_init_field` / `dictionary_init_message`** moved from
  `nanofix/detail/fields.hpp` to the opt-in `nanofix/names.hpp` (header-location change,
  not an API removal). Include `<nanofix/names.hpp>` to use them.

## Porting example

```cpp
// upstream
try {
    int seq = field.as_int<int>();               // no signal on bad input
    writer.push_back_string(tag, std::string(s));
    writer.push_back_trailer();
} catch (std::exception const&) { /* full buffer, misuse */ }

// here
int seq;
if (!field.try_as_int<int>(seq)) return reject();
writer.push_back_string(tag, s);                 // string_view, no copy
if (!writer.push_back_trailer()) return too_small();
```

## Build and tooling

- C++20 required; the old `__cplusplus` guards are gone.
- CMake + Conan 2. Tests use GoogleTest, benchmarks use Google Benchmark.
- `cmake --install` exports a relocatable CMake package
  (`find_package(nanofix CONFIG)` → `nanofix::nanofix` + `nanofix_generate()`),
  so plain-CMake consumers do not need Conan.
- `fixspec-gen` was rewritten from Haskell to C++ (pugixml) and takes
  QuickFIX-format XML (`fixspec/FIX50SP2.xml` + `FIXT11.xml`). No Cabal.
- `fixgen` generates the synthetic dataset the benchmarks run on.
- Why the fork is faster (no behavior change): O(1) `is_tag_a_data_length`
  bitmap, SWAR timestamp parsing, SIMD bulk framing (`find_all_soh`), tag
  search (`find_tag_in_index`) and `checksum_bytes` (scalar fallback,
  `NANOFIX_DISABLE_SIMD` to force it), and an iterator that carries its own
  `message_end`. Benchmark on your target with `compare2upstream/run.sh`.
