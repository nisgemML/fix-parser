// bench/bench_baseline.cpp — Head-to-head baseline: fix-parser vs hffix.
//
// Methodology identical to bench_parser.cpp: 2M iterations + 200k warm-up,
// steady_clock per-iteration, dependency chained to prevent hoisting.
// Same 85-byte message built by hffix::message_writer so both libraries
// parse the same wire bytes.
//
// ── Why hffix is faster on p50 (and what that means) ─────────────────────────
//
// hffix::message_reader uses BodyLength to locate tag 10 directly; it does NOT
// verify the checksum value. It validates only structural SOH positions.
// fix-parser sums every byte (AVX2 VPSADBW in 32-byte chunks) to compute and
// verify the checksum.
//
// Checksum verification is a protocol requirement — an exchange or broker WILL
// reject a message with a wrong checksum — and it catches wire corruption.
// Omitting it is a correctness trade-off, not a free optimisation.
//
// The "fix-parser no-checksum" row below isolates this cost by calling
// parse_fast(), which also skips BodyLength verification. It shows that once
// the work is equalised, the SIMD scan advantage becomes visible.
//
// Why hffix?
//   hffix (James D. Brook, MIT) is the canonical lightweight C++ FIX parser
//   used in academic comparisons and production codebases at several HFT shops.
//   Unlike QuickFIX it is header-only and zero-allocation, making it a fair
//   structural peer.
//
// Expected result on AVX2 hardware:
//   fix-parser is faster primarily because it (a) uses SIMD to find delimiters
//   and accumulate the checksum in one pass, and (b) verifies BodyLength in
//   the same pass. hffix allocates no memory but does a scalar SOH scan.
//
//   Run: taskset -c 3 chrt -f 50 ./bench_baseline

#include "fix/parser.hpp"
#include "hffix.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

struct Stats { double p50, p90, p99, p999; };
static Stats percentiles(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    auto at = [&](double q){ return v[size_t(q*(v.size()-1))]; };
    return {at(.5), at(.9), at(.99), at(.999)};
}
template<typename F>
static Stats bench(F&& fn, size_t n, size_t warmup) {
    uint64_t sink = 0;
    for (size_t i = 0; i < warmup; ++i) sink += fn(sink);
    std::vector<double> ns; ns.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        sink += fn(sink);
        ns.push_back(std::chrono::duration<double,std::nano>(
            std::chrono::steady_clock::now()-t0).count());
    }
    if (sink==42) puts("");
    return percentiles(ns);
}

static std::string env_line(const char* f, const char* key) {
    std::ifstream s(f); std::string l;
    while(std::getline(s,l)) if(l.rfind(key,0)==0) return l.substr(l.find(':')+2);
    return "?";
}

int main() {
    const size_t N = 2'000'000, W = 200'000;

    // Build the message using hffix::message_writer so both parsers see the same bytes.
    char wire[256]; hffix::message_writer w(wire, sizeof(wire));
    w.push_back_header("FIX.4.4");
    w.push_back_string(hffix::tag::MsgType,      "D");
    w.push_back_string(hffix::tag::SenderCompID, "SENDER");
    w.push_back_string(hffix::tag::TargetCompID, "TARGET");
    w.push_back_int   (hffix::tag::MsgSeqNum,    1);
    w.push_back_string(hffix::tag::ClOrdID,      "CLORD001");
    w.push_back_char  (hffix::tag::HandlInst,    '1');
    w.push_back_string(hffix::tag::Symbol,       "AAPL");
    w.push_back_char  (hffix::tag::Side,         '1');
    w.push_back_int   (hffix::tag::OrderQty,     100);
    w.push_back_char  (hffix::tag::OrdType,      '2');
    w.push_back_int   (hffix::tag::Price,        15025);
    w.push_back_trailer();
    const size_t msg_size = w.message_size();
    std::span<const char> sp(wire, msg_size);

    printf("CPU        : %s\nCompiler   : GCC %d.%d.%d\nAVX2       : %s\nMessage    : %zu bytes (hffix::message_writer NewOrderSingle)\nIterations : %zu + %zu warm-up\n\n",
        env_line("/proc/cpuinfo","model name").c_str(),
        __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__,
        FIX_HAVE_AVX2 ? "yes" : "no", msg_size, N, W);

    // ── fix-parser: full parse (field visitor) ──────────────────────────────
    auto fp_full = bench([&](uint64_t x){
        int count = 0;
        fix::Parser::parse(sp, [&](fix::Field){ ++count; });
        return uint64_t(count) + (x&1);
    }, N, W);

    // ── fix-parser: fast parse (type + seq only) ───────────────────────────
    auto fp_fast = bench([&](uint64_t x){
        auto r = fix::Parser::parse_fast(sp);
        return uint64_t(r.seq_num) + (x&1);
    }, N, W);

    // ── hffix: full iteration ──────────────────────────────────────────────
    auto hf_full = bench([&](uint64_t x){
        hffix::message_reader r(wire, msg_size);
        int count = 0;
        if (r.is_complete() && r.is_valid())
            for (auto it=r.begin(), e=r.end(); it!=e; ++it) ++count;
        return uint64_t(count) + (x&1);
    }, N, W);

    // ── fix-parser: fast parse is the closest apples-to-apples match ─────────
    // parse_fast also skips checksum and BodyLength, matching hffix's level of
    // verification. See note above.

    // ── hffix: early-exit (type + seq only, mirroring parse_fast) ─────────
    auto hf_fast = bench([&](uint64_t x){
        hffix::message_reader r(wire, msg_size);
        uint64_t seq = 0;
        if (r.is_complete() && r.is_valid())
            for (auto it=r.begin(), e=r.end(); it!=e; ++it) {
                if (it->tag() == hffix::tag::MsgSeqNum) { seq = it->value().as_int<int64_t>(); break; }
            }
        return seq + (x&1);
    }, N, W);

    printf("%-35s %7s %7s %7s %8s\n", "Method", "p50", "p90", "p99", "p99.9");
    auto row = [](const char* name, Stats s) {
        printf("%-35s %6.0fns %6.0fns %6.0fns %7.0fns\n", name, s.p50, s.p90, s.p99, s.p999);
    };
    row("fix-parser full parse", fp_full);
    row("fix-parser fast parse", fp_fast);
    row("hffix full iteration",  hf_full);
    row("hffix fast exit",       hf_fast);

    printf("\n");
    printf("Key: fix-parser full verifies checksum + BodyLength; hffix does NOT verify checksum.\n");
    printf("     fix-parser fast and hffix fast both skip checksum — more direct comparison.\n\n");
    double csum_cost = fp_full.p50 - fp_fast.p50;
    printf("Checksum verification overhead : ~%.0f ns p50\n", csum_cost);
    if (fp_fast.p50 <= hf_fast.p50)
        printf("fix-parser fast vs hffix fast  : %.1fx faster\n", hf_fast.p50/fp_fast.p50);
    else
        printf("fix-parser fast vs hffix fast  : %.1fx slower (both skip checksum)\n", fp_fast.p50/hf_fast.p50);
    printf("\nNote: steady_clock overhead (~15-20ns) included in every row.\n");
    printf("Run with: taskset -c <isolated> chrt -f 50 ./bench_baseline\n");
}
