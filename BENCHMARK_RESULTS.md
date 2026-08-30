# Benchmark Results

Every row here was produced by `bench/bench_parser.cpp` and pasted verbatim.
Re-run on your own hardware before quoting a number; results differ by 2× across
CPU generations.

## Run 1 — shared cloud container (NOT an isolated core)

```
CPU        : Intel(R) Xeon(R) Processor @ 2.10GHz
Compiler   : GCC 13.3.0, -O3 -march=native
AVX2       : yes
Message    : 150 bytes NewOrderSingle, 171 bytes NewOrderMultileg
Iterations : 2000000 (+200k warm-up)
Isolation  : none (container, no taskset/chrt)

Method                           p50     p90     p99   p99.9
Full parse (all fields)         141ns   143ns   144ns   208ns
Fast parse (type+seq)            75ns    76ns    81ns   101ns
Typed NewOrderSingle            189ns   199ns   350ns   570ns
Group parse (multileg)          549ns   559ns   771ns  4266ns
```

Notes:
- Full parse now includes BodyLength verification and the data-field table
  lookup, which the previous version did not do. The ~20 ns of per-iteration
  `steady_clock` overhead is included in every row.
- Max values (not shown) were 200–500 µs: container scheduler preemption.
  This is exactly why the isolated-core run below is the one to quote.

## Run 2 — isolated core

_Not yet recorded. Run `taskset -c <core> chrt -f 50 ./build/bench_parser`
on a machine with `isolcpus`/`nohz_full` and paste the output here._

## Test results

```
test_parser  : 48 passed, 0 failed   (incl. SIMD/scalar equivalence on 5000 random buffers)
test_groups  : 91 passed, 0 failed   (nested NoLegs→NoNestedPartyIDs, MD incremental, count mismatch)
test_session : 31 passed, 0 failed
ASan + UBSan : clean
```

## Baseline vs hffix — same machine, same message

```
Message : 107 bytes NewOrderSingle (built by hffix::message_writer)
Container, no core isolation.

Method                                  p50     p90     p99    p99.9
fix-parser full parse (+ checksum)     126ns   128ns   142ns   208ns
fix-parser fast parse (no checksum)     70ns    72ns    85ns   178ns
hffix full iteration (no checksum)     100ns   101ns   103ns   155ns
hffix fast exit (no checksum)           53ns    55ns    65ns   114ns
```

hffix does not verify the checksum; fix-parser full does.
Checksum cost: ~52 ns p50. See PROFILING.md for analysis.
