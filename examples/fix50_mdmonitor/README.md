# fix50_mdmonitor

A FIX 5.0 market-data monitor. It writes a few `MarketDataSnapshotFullRefresh`
('W') and `MarketDataIncrementalRefresh` ('X') messages with `message_writer`,
then reads them back and prints each `NoMDEntries` entry. Self-contained, no
sample file.

It demonstrates the **per-MsgType repeating group**. Both messages carry a
`NoMDEntries` group, but its delimiter differs: snapshot entries start at
`MDEntryType` (269), incremental entries at `MDUpdateAction` (279). The monitor
feeds the matching `(count, delim)` pair to the runtime `group(count, delim)`
overload, then pulls each entry's fields with the per-entry `find_with_hint`.
`MDEntryType` / `MDUpdateAction` are decoded with `nanofix::value_name` from the
opt-in `nanofix/names.hpp`.

## Layout

```
fix50_mdmonitor/
├── CMakeLists.txt
├── conanfile.txt
└── src/main.cpp
```

## Build

```sh
# Publish nanofix once (from the repo root).
conan create . --build=missing \
    -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20 \
    -c tools.build:skip_test=True

# Build this example.
cd examples/fix50_mdmonitor
conan install . --output-folder=build --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20
cmake --preset conan-release
cmake --build build/build/Release -j
./build/build/Release/fix50_mdmonitor
```

Output:

```
SNAPSHOT KO
    BID    px=60.12      size=500
    OFFER  px=60.15      size=300
INCREMENTAL
    NEW    OFFER  AAPL   px=190.12     size=120
    DELETE BID    MSFT   px=410.50     size=80
...
---
2 snapshots, 2 incrementals
```
