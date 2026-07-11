# fix44_gateway

Standalone Conan 2 project consuming `nanofix`. Built against a custom
FIX 4.4 spec (`fixspec/FIX44.xml`, taken verbatim from
[quickfix/quickfix](https://github.com/quickfix/quickfix)). This
directory is self-contained — it does NOT participate in the parent
`nanofix` CMake build; treat it as an external downstream consumer.

Writes two `NewOrderSingle` and one `ExecutionReport` into a fixed
buffer, parses the buffer back with `for_each_message` behind an ingress
checksum gate (`calculate_check_sum()` vs the wire CheckSum — framing
validation alone never sums the bytes), and pulls business fields out via
`with_fields` (index-or-iterator, decided once).
`CMakeLists.txt` calls `nanofix_generate()` against the spec, so the generated
`nanofix/detail/fields.hpp` and `names.hpp` for this binary are FIX 4.4, not the FIX
5.0 SP2 set that ships with nanofix.

## Run

```sh
./fix44_gateway            # parse + print
./fix44_gateway out.fix44  # parse + print + dump
```

## Build

Publish `nanofix` to the local Conan cache once (from the repo root):

```sh
conan create . --build=missing \
    -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20 \
    -c tools.build:skip_test=True
```

Then build the example:

```sh
cd examples/fix44_gateway
conan install . --output-folder=build --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20
cmake --preset conan-release
cmake --build build/build/Release -j
./build/build/Release/fix44_gateway
```

The `nanofix` Conan package ships `fixspec-gen` and
`nanofix-generate.cmake`, so the codegen step finds the binary
automatically.
