# fix-parser

**Zero-copy FIX 4.2/4.4 parser in C++20 — flat fields, length-prefixed data
fields, and dictionary-driven repeating groups, with no heap allocation.**

Parses FIX `tag=value\001` wire format from a `std::span<const char>` view of
the receive buffer. Every parsed field is a `std::string_view` into that
buffer. Repeating groups (including nested ones) are resolved against a
dictionary generated from the FIX 4.4 XML spec at build time.

## What it handles

| Layer | Status |
|---|---|
| Flat `tag=value` scan, checksum, BodyLength verification | ✅ `parser.hpp` |
| Length-prefixed data fields (RawData, XmlData, SecureData, EncodedText…) that legally contain SOH | ✅ `parser.hpp` |
| Repeating groups, nested to any depth ≤ 8, per-message delimiters | ✅ `groups.hpp` + generated `dictionary_fix44.hpp` |
| Session layer: logon/logout, heartbeat, test request, resend, sequence reset | ✅ `session.hpp` |
| Typed decoders | NewOrderSingle (D), ExecutionReport (8) |
| Fuzzing | libFuzzer harness + seed corpus (`fuzz/`) |

See [LIMITATIONS.md](LIMITATIONS.md) for what is deliberately out of scope.

## Performance

Numbers below are produced by `bench/bench_parser.cpp`, which prints the CPU,
compiler, and AVX2 status it ran with. Do not trust a number in this file that
is not also in [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) with its
environment attached. See [PROFILING.md](PROFILING.md) for the baseline comparison against hffix,
the checksum cost analysis, and instructions for perf counters and flame graphs.
Run the benchmarks yourself:

```bash
cmake -S . -B build && cmake --build build
taskset -c 3 chrt -f 50 ./build/bench_parser        # pinned core, RT priority
./build/bench_parser --json                          # machine-readable
./build/bench_baseline                               # vs hffix
```

## Design

**Zero-copy, zero-alloc.** Parser state lives on the stack; values are views
into the wire buffer.

**Vectorised scan.** Delimiters are located with AVX2 `VPCMPEQB`/`VPMOVMSKB`
(32 bytes per compare) and the checksum accumulated with `VPSADBW`. A scalar
path is the reference implementation; tests assert the two agree byte-for-byte
on random buffers, and a non-AVX2 build is slower but never different.

**Checksum in one pass.** Each field's bytes are summed as it is consumed;
tag 10 is excluded by construction, so there is no subtract-at-end.

**Length-prefixed data fields.** A SOH-scanning parser desyncs on the first
`RawData` payload containing `\001`. This parser tracks the Length/Data tag
pairs (95/96, 212/213, 90/91, 354/355, …) and reads the data field by its
declared length. Declared length longer than the wire is a parse error, not an
out-of-bounds read.

**BodyLength is verified.** Tag 9 must equal the byte count between its SOH
and `10=`; a truncated or concatenated message is rejected before the
checksum comparison.

**Repeating groups without a heap.** `GroupParser` assigns each field a path —
the stack of `(count_tag, entry_index)` it sits inside — using the flat
visitor and a fixed-depth frame stack. A new entry starts when a delimiter
tag appears with no entry open, or when a tag repeats within the current
entry (tags are unique per entry, so a repeat can only mean "next entry").
The dictionary knows that `NoMDEntries` is delimited by 269 in a Snapshot
and 279 in an Incremental Refresh.

**`parse_fast()`.** Stops after tag 35 and tag 34 with no checksum, for
sequence-gap detection before committing to a full parse.

**Branch-light, not branch-free.** Integer decode is `std::from_chars`.
Anything that validates digits branches.

## Usage

```cpp
#include "fix/groups.hpp"
#include "fix/dictionary_fix44.hpp"

std::span<const char> wire = /* raw bytes from the socket */;

// Flat visitor
auto r = fix::Parser::parse(wire, [](fix::Field f) {
    printf("tag=%d value=%.*s\n", f.tag, int(f.value.size()), f.value.data());
});
if (r.error != fix::ParseError::Ok) puts(fix::to_string(r.error));

// Typed decode
if (auto o = fix::NewOrderSingle::from(wire))
    printf("%.*s qty=%ld\n", int(o->symbol.size()), o->symbol.data(), o->order_qty);

// Repeating groups: each field arrives with its group path
fix::GroupParser::parse(wire, fix::fix44::kDictionary,
    [](fix::Field f, const fix::GroupPath& p) {
        // p.depth == 0 → top level; p.elems[0] == {453, 1} → NoPartyIDs entry 1
    });

// Or materialise (fixed capacity, still no heap)
auto m = fix::Message<>::from(wire, fix::fix44::kDictionary);
auto party1 = m.get(453, 1, 448);   // PartyID of the second party entry
```

## Build & test

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
cmake -S . -B build-asan -DFIX_SANITIZERS=ON            # ASan + UBSan
CXX=clang++ cmake -S . -B build-fuzz -DFIX_FUZZ=ON && cmake --build build-fuzz
./build-fuzz/fuzz_parser fuzz/corpus -max_len=4096
python3 tools/gen_dictionary.py spec/FIX44.xml > include/fix/dictionary_fix44.hpp   # regenerate
```
