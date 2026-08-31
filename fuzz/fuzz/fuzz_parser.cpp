// fuzz/fuzz_parser.cpp — libFuzzer harness for the flat parser, group parser,
// and both typed decoders. Any crash, sanitizer report, or timeout is a bug.
//
// Build:  cmake -DFIX_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++ ..
// Run:    ./fuzz_parser ../fuzz/corpus -max_len=4096 -timeout=5
//
// Invariants checked in addition to "does not crash":
//   1. Every visited string_view lies inside the input buffer.
//   2. field_count equals the number of visitor calls.
//   3. parse_fast never reports a MsgType/SeqNum that parse disagrees with.
//   4. SIMD and scalar checksum agree on the input.
#include "fix/groups.hpp"
#include "fix/dictionary_fix44.hpp"
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::span<const char> msg(reinterpret_cast<const char*>(data), size);
    const char* lo = msg.data();
    const char* hi = lo + size;

    int visits = 0;
    auto r = fix::Parser::parse(msg, [&](fix::Field f) {
        ++visits;
        if (f.value.data() < lo || f.value.data() + f.value.size() > hi) __builtin_trap();
    });
    if (r.error == fix::ParseError::Ok && visits != r.field_count) __builtin_trap();

    auto fast = fix::Parser::parse_fast(msg);
    if (r.error == fix::ParseError::Ok && fast.error == fix::ParseError::Ok) {
        if (fast.msg_type_char != r.msg_type_char) __builtin_trap();
        if (fast.seq_num != r.seq_num) __builtin_trap();
    }

    if (fix::simd::sum_bytes(lo, hi) != fix::simd::sum_bytes_scalar(lo, hi)) __builtin_trap();

    (void)fix::GroupParser::parse(msg, fix::fix44::kDictionary, [](fix::Field, const fix::GroupPath&){});
    (void)fix::Message<64>::from(msg, fix::fix44::kDictionary);
    (void)fix::NewOrderSingle::from(msg);
    (void)fix::ExecutionReport::from(msg);
    return 0;
}
