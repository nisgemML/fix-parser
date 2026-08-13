# fix-parser

**Zero-copy FIX 4.2/4.4 message parser in C++20.**

Parses FIX `tag=value\001` wire format with no heap allocation and no data copies.
Operates on a `std::span<const char>` view of the raw receive buffer — parsed
string fields return `std::string_view` pointing into the original buffer.

## Performance

| Method | p50 | p90 | p99 |
|--------|-----|-----|-----|
| Full parse (all fields) | **112 ns** | 158 ns | 220 ns |
| Fast parse (type+seq only) | **60 ns** | 61 ns | 112 ns |

Measured on a 171-byte `NewOrderSingle`. See [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md).

## Design

**Zero-copy:** `std::string_view` fields point directly into the wire buffer.
No `memcpy`, no `std::string`, no dynamic allocation on the parse path.

**Checksum in one pass:** Accumulated as bytes are consumed — no second scan.
Tag 10 bytes are subtracted at the end.

**`parse_fast()`:** Stops after finding tag 35 (MsgType) and tag 34 (SeqNum).
Used for sequence gap detection before committing to a full parse — saves ~52ns
per message at 1M msg/sec = 52ms CPU/sec.

**`__builtin_expect`:** SOH delimiter is rare relative to value bytes.
Tells the branch predictor to predict "not SOH" — correct ~95% of the time.

## Usage

```cpp
#include "fix/parser.hpp"

// Full parse — visit all fields
std::string wire = /* raw FIX message */;
auto span = std::span<const char>(wire.data(), wire.size());

auto result = fix::Parser::parse(span, [](fix::Field f) {
    printf("tag=%d value=%.*s\n", f.tag, int(f.value.size()), f.value.data());
});

// Typed decode — NewOrderSingle
auto order = fix::NewOrderSingle::from(span);
if (order) {
    printf("symbol=%s qty=%ld\n",
           std::string(order->symbol).c_str(), order->order_qty);
}

// Fast parse — MsgType + SeqNum only (60ns)
auto fast = fix::Parser::parse_fast(span);
printf("type=%c seq=%u\n", char(fast.msg_type_char), fast.seq_num);
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure    # 35 tests, 0 failures
```

## Jump Trading relevance

FIX is the order management protocol at most trading firms including Jump Trading.
The interview question "parse this FIX message as fast as possible" has a
well-known answer: zero-copy span, checksum accumulated in place, `parse_fast()`
for sequence gap detection. This repo is that answer in working code.
