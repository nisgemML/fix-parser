// bench/bench_parser.cpp — Latency benchmark. Prints the environment it ran in
// so the numbers in BENCHMARK_RESULTS.md are reproducible, not asserted.
//
//   taskset -c 3 chrt -f 50 ./bench_parser            # pinned, RT priority
//   ./bench_parser --json > results.json               # machine-readable
//
// Methodology: N=2,000,000 iterations per method after 200k warm-up; each
// iteration parses a 171-byte NewOrderSingle already in L1; rdtsc-free
// (steady_clock) per-iteration timing; percentiles from the full sample,
// not a histogram. A dependency on the parse result is fed back into the
// next iteration so the compiler cannot hoist the work.
#include "fix/groups.hpp"
#include "fix/dictionary_fix44.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static std::string build(const char* body_pipe) {
    std::string body; for (const char* p = body_pipe; *p; ++p) body += (*p == '|') ? '\x01' : *p;
    std::string m = "8=FIX.4.4\x01" "9=" + std::to_string(body.size()) + "\x01" + body;
    char buf[16]; snprintf(buf, sizeof buf, "10=%03d\x01", int(fix::compute_checksum({m.data(), m.size()})));
    return m + buf;
}

struct Stats { double p50, p90, p99, p999, max; };
static Stats stats(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[size_t(q * (v.size() - 1))]; };
    return {at(.50), at(.90), at(.99), at(.999), v.back()};
}

template<typename F>
static Stats run(F&& fn, size_t n) {
    std::vector<double> ns; ns.reserve(n);
    uint64_t sink = 0;
    for (size_t i = 0; i < 200'000; ++i) sink += fn(sink);
    for (size_t i = 0; i < n; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        sink += fn(sink);
        auto t1 = std::chrono::steady_clock::now();
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    if (sink == 42) puts("");   // keep sink alive
    return stats(ns);
}

static std::string env_line(const char* path, const char* key) {
    std::ifstream f(path); std::string line;
    while (std::getline(f, line)) if (line.rfind(key, 0) == 0) return line.substr(line.find(':') + 2);
    return "?";
}

int main(int argc, char** argv) {
    bool json = argc > 1 && !strcmp(argv[1], "--json");
    const size_t N = 2'000'000;
    std::string nos = build("35=D|49=SENDER|56=TARGET|34=1|52=20230901-09:30:00.000|11=CLORD001|21=1|55=AAPL|54=1|60=20230901-09:30:00|38=100|40=2|44=15025|");
    std::string ml  = build("35=AB|49=A|56=B|34=1|11=ML1|55=SPY|54=1|38=10|40=2|555=2|600=SPY|624=1|623=1|600=SPY|624=2|623=1|539=2|524=NP1|525=D|538=1|524=NP2|525=D|538=2|60=T|");
    std::span<const char> s1(nos.data(), nos.size()), s2(ml.data(), ml.size());

    auto full  = run([&](uint64_t x){ auto r = fix::Parser::parse(s1, [](fix::Field){}); return uint64_t(r.field_count) + (x & 1); }, N);
    auto fast  = run([&](uint64_t x){ auto r = fix::Parser::parse_fast(s1); return uint64_t(r.seq_num) + (x & 1); }, N);
    auto typed = run([&](uint64_t x){ auto o = fix::NewOrderSingle::from(s1); return uint64_t(o ? o->order_qty : 0) + (x & 1); }, N);
    auto grp   = run([&](uint64_t x){ auto r = fix::GroupParser::parse(s2, fix::fix44::kDictionary, [](fix::Field, const fix::GroupPath&){}); return uint64_t(r.parse.field_count) + (x & 1); }, N);

    std::string cpu = env_line("/proc/cpuinfo", "model name");
    if (json) {
        printf("{\"cpu\":\"%s\",\"compiler\":\"%s\",\"avx2\":%d,\"msg_bytes\":%zu,\"iterations\":%zu,\n",
               cpu.c_str(), __VERSION__, FIX_HAVE_AVX2, nos.size(), N);
        auto row = [](const char* k, Stats s, bool last) {
            printf(" \"%s\":{\"p50\":%.0f,\"p90\":%.0f,\"p99\":%.0f,\"p999\":%.0f,\"max\":%.0f}%s\n", k, s.p50, s.p90, s.p99, s.p999, s.max, last ? "" : ",");
        };
        row("full_parse", full, false); row("fast_parse", fast, false); row("typed_nos", typed, false); row("group_parse_multileg", grp, true);
        printf("}\n");
        return 0;
    }
    printf("CPU        : %s\nCompiler   : %s\nAVX2       : %s\nMessage    : %zu bytes NewOrderSingle, %zu bytes NewOrderMultileg\nIterations : %zu (+200k warm-up)\n\n",
           cpu.c_str(), __VERSION__, FIX_HAVE_AVX2 ? "yes" : "no", nos.size(), ml.size(), N);
    printf("%-28s %7s %7s %7s %7s %8s\n", "Method", "p50", "p90", "p99", "p99.9", "max");
    auto row = [](const char* k, Stats s) { printf("%-28s %6.0fns %6.0fns %6.0fns %6.0fns %7.0fns\n", k, s.p50, s.p90, s.p99, s.p999, s.max); };
    row("Full parse (all fields)", full);
    row("Fast parse (type+seq)", fast);
    row("Typed NewOrderSingle", typed);
    row("Group parse (multileg)", grp);
}
