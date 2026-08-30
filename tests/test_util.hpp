#pragma once
// tests/test_util.hpp — shared helpers for building wire-correct FIX messages.
#include "fix/parser.hpp"
#include <cstdio>
#include <string>

inline int g_passed = 0, g_failed = 0;

#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++g_failed; \
    } else { ++g_passed; } } while(0)

// '|' → SOH for readable test literals.
inline std::string soh(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p) out += (*p == '|') ? '\x01' : *p;
    return out;
}

// Build a complete, wire-correct message from the body (everything after
// tag 9, excluding tag 10). BodyLength and CheckSum are computed for real.
inline std::string build(const std::string& body, const char* begin = "FIX.4.4") {
    std::string msg = std::string("8=") + begin + "\x01" + "9=" +
                      std::to_string(body.size()) + "\x01" + body;
    uint8_t cs = fix::compute_checksum({msg.data(), msg.size()});
    char buf[16];
    snprintf(buf, sizeof(buf), "10=%03d\x01", int(cs));
    return msg + buf;
}

inline std::span<const char> sp(const std::string& s) { return {s.data(), s.size()}; }
