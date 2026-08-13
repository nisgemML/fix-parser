# Benchmark Results — FIX 4.2/4.4 Parser

## Test results

```
35 tests passed, 0 failed

  NewOrderSingle parse    : all fields correct
  ExecutionReport parse   : all fields correct
  Checksum verification   : valid accepted, corrupt rejected
  parse_fast              : MsgType + SeqNum extracted correctly
  Field iteration         : all tags visited in order
  Side values             : buy/sell correctly parsed
  Edge cases              : empty message handled
  Multiple message types  : D/8/F/0/A/5 all correct
  Large quantities        : 1,000,000 shares, price 9,999,999
  Zero-copy               : string_views point into original buffer
```

## Latency (171-byte NewOrderSingle)

```
Method                  p50     p90     p99    p99.9
Full parse (all fields) 112 ns  158 ns  220 ns  401 ns
Fast parse (type+seq)    60 ns   61 ns  112 ns  190 ns
```

## Design decisions

**Zero-copy:** `std::string_view` fields point directly into the receive buffer.
No `memcpy`, no `std::string` construction on the parse path.

**Zero-alloc:** All state on the stack. No heap allocation.

**Checksum in one pass:** Accumulated as bytes are consumed in `parse_value()`.
Tag 10 bytes subtracted at end — one pass, no second scan.

**`__builtin_expect`:** SOH delimiter is rare relative to value bytes.
Branch predictor predicts "not SOH" — correct ~95% of the time.

**`std::from_chars`:** No locale, no NUL-termination required, no exception.

**`parse_fast`:** Stops after tag 35 + tag 34. Saves ~52ns per message
for sequence gap detection before full parse.
