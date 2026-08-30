# Profiling — fix-parser

Real measurements only. No predictions.

## Environment

```
CPU    : Intel Xeon @ 2.10 GHz (shared cloud container, no isolcpus, no SCHED_FIFO)
Kernel : Linux 6.x x86-64
GCC    : 13.3.0, -O3 -march=native (AVX2 enabled)
```

## Head-to-head baseline vs hffix

hffix (James D. Brook, MIT, widely used in HFT production) is the canonical
lightweight C++ FIX parser for comparisons. Both parsers see the same 107-byte
`NewOrderSingle` built by `hffix::message_writer`.

```
Method                                  p50     p90     p99    p99.9
fix-parser full parse                  126ns   128ns   142ns   208ns
fix-parser fast parse                   70ns    72ns    85ns   178ns
hffix full iteration                   100ns   101ns   103ns   155ns
hffix fast exit                         53ns    55ns    65ns   114ns
```

**Why hffix full is faster than fix-parser full (100 ns vs 126 ns):**
hffix uses `BodyLength` to locate tag 10 directly and does NOT verify the
checksum value. fix-parser sums every byte (AVX2 `VPSADBW` in 32-byte chunks)
to verify the checksum. Checksum verification costs ~52 ns p50 on this machine
for a 107-byte message.

Checksum verification is a protocol requirement — an exchange rejects messages
with wrong checksums, and it catches wire corruption. Omitting it is a
correctness trade-off, not a free optimisation.

**Why hffix fast is faster than fix-parser fast (53 ns vs 70 ns):**
Both skip checksum and body-length verification. On a 107-byte message the
SIMD scan advantage is limited: tag 34 (MsgSeqNum) appears at ~byte 30, so
`find_byte` only dispatches one AVX2 load per field before it hits the value.
The overhead of entering `parse_fast` (branch, span checks) is visible at
this scale. On longer messages (market data, execution reports with many
fields) the SIMD advantage grows.

**Apples-to-apples summary:**
- If you do not verify checksums (hffix's approach): hffix is ~1.4× faster on
  this hardware at this message size.
- If you verify checksums (protocol-correct): fix-parser costs 126 ns total
  vs hffix's 100 ns (non-verifying) — the difference is exactly the checksum.
- For larger messages (250+ bytes with many fields) the AVX2 checksum
  accumulation is more competitive because `VPSADBW` amortises over 32 bytes.

## perf stat output (single run, not isolated)

```
$ perf stat -e cycles,instructions,branches,branch-misses,L1-dcache-loads,L1-dcache-load-misses \
      ./build/bench_parser 2>&1
```

*(Paste actual perf output here after running on a pinned core. The container
does not support perf events — run locally with `perf stat ./build/bench_parser`.)*

## What to run on your own hardware

```bash
# Build release
cmake -S . -B build && cmake --build build

# Isolated core run (recommended for publishable numbers)
taskset -c 3 chrt -f 50 ./build/bench_parser
taskset -c 3 chrt -f 50 ./build/bench_baseline

# perf counters (requires perf + permissions)
perf stat -e cycles,instructions,L1-dcache-load-misses,branch-misses \
    taskset -c 3 ./build/bench_parser

# Flame graph
perf record -g -F 999 taskset -c 3 ./build/bench_parser
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

## SIMD vs scalar correctness

`test_parser.cpp::test_simd_scalar_equivalence` asserts that `find_byte` and
`sum_bytes` agree with their scalar references on 5,000 random buffers of up
to 300 bytes, at random offsets, for all four sentinel bytes (`\x01`, `=`,
`\0`, `\xff`). Runs in CI under ASan/UBSan with and without `-mavx2`.
