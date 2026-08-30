#pragma once
// include/fix/parser.hpp — Zero-copy FIX 4.2/4.4 message parser.
//
// Parses FIX tag=value\001 wire format without heap allocation.
// Operates on a std::span<const char> view of the raw wire buffer —
// no copies, no std::string, no dynamic allocation on the hot path.
//
// ── FIX wire format ──────────────────────────────────────────────────────────
//
//   8=FIX.4.2\001 9=178\001 35=D\001 49=SENDER\001 56=TARGET\001
//   11=CLORD001\001 21=1\001 55=AAPL\001 54=1\001 60=20230901-09:30:00\001
//   38=100\001 40=2\001 44=15025\001 10=247\001
//
// Tag 8:  BeginString  (FIX.4.2 or FIX.4.4)
// Tag 9:  BodyLength   (bytes after tag-9 SOH up to and excluding "10=")
// Tag 35: MsgType      (D=NewOrderSingle, 8=ExecutionReport, F=OrderCancel)
// Tag 10: CheckSum     (sum of all bytes mod 256, zero-padded to 3 digits)
//
// ── Why this design ──────────────────────────────────────────────────────────
//
// 1. Zero-copy: parser holds a span<const char> into the receive buffer.
//    No data is copied. Parsed string fields return string_view into buffer.
//
// 2. Zero allocation: all state is on the stack or in the span.
//
// 3. Branch-light integer decode: parse_int() uses std::from_chars (no locale,
//    no NUL termination, no exceptions). It is not branch-free — nothing that
//    validates digits is.
//
// 4. Checksum in one pass: each field's bytes are summed as it is consumed
//    (vectorised, see simd.hpp). Tag 10 is excluded by construction, so there
//    is no subtract-at-end step.
//
// 5. Length-prefixed data fields are honoured. RawData (96), XmlData (213),
//    SecureData (91), Signature (89), EncodedText (355) etc. may legally
//    contain SOH bytes; their length is announced by the preceding Length tag
//    (95, 212, 90, 93, 354). A SOH-scanning parser that ignores this desyncs
//    on the first binary payload. See kDataFieldPairs.
//
// 6. BodyLength (tag 9) is verified against the observed body span, so a
//    truncated or concatenated message is rejected before the checksum step.
//
// Repeating groups are handled in groups.hpp on top of this flat visitor.

#include "fix/simd.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace fix {

static constexpr char kSOH = '\x01';

struct Tag {
    static constexpr int BeginString  = 8;
    static constexpr int BodyLength   = 9;
    static constexpr int MsgType      = 35;
    static constexpr int SenderCompID = 49;
    static constexpr int TargetCompID = 56;
    static constexpr int MsgSeqNum    = 34;
    static constexpr int SendingTime  = 52;
    static constexpr int ClOrdID      = 11;
    static constexpr int HandlInst    = 21;
    static constexpr int Symbol       = 55;
    static constexpr int Side         = 54;
    static constexpr int TransactTime = 60;
    static constexpr int OrderQty     = 38;
    static constexpr int OrdType      = 40;
    static constexpr int Price        = 44;
    static constexpr int OrderID      = 37;
    static constexpr int ExecID       = 17;
    static constexpr int ExecType     = 150;
    static constexpr int OrdStatus    = 39;
    static constexpr int LeavesQty    = 151;
    static constexpr int CumQty       = 14;
    static constexpr int AvgPx        = 6;
    static constexpr int CheckSum     = 10;
    // Length/data pairs
    static constexpr int SecureDataLen  = 90,  SecureData  = 91;
    static constexpr int SignatureLen   = 93,  Signature   = 89;
    static constexpr int RawDataLength  = 95,  RawData     = 96;
    static constexpr int XmlDataLen     = 212, XmlData     = 213;
    static constexpr int EncodedTextLen = 354, EncodedText = 355;
};

// Length tag → data tag. Sorted by length tag; small enough to scan linearly.
struct DataFieldPair { int length_tag; int data_tag; };
inline constexpr DataFieldPair kDataFieldPairs[] = {
    {90, 91}, {93, 89}, {95, 96}, {212, 213}, {354, 355},
    {348, 349}, {350, 351}, {352, 353}, {356, 357}, {358, 359},
    {360, 361}, {362, 363}, {364, 365}, {445, 446},
};

[[nodiscard]] constexpr int data_tag_for_length(int length_tag) noexcept {
    for (const auto& p : kDataFieldPairs)
        if (p.length_tag == length_tag) return p.data_tag;
    return -1;
}

enum class ParseError : uint8_t {
    Ok,
    EmptyMessage,
    MissingBeginString,
    InvalidBodyLength,
    ChecksumMismatch,
    UnexpectedEnd,
    MalformedTag,
    MalformedValue,
    MalformedDataField,   // data field length disagrees with wire
};

[[nodiscard]] constexpr const char* to_string(ParseError e) noexcept {
    switch (e) {
        case ParseError::Ok:                 return "Ok";
        case ParseError::EmptyMessage:       return "EmptyMessage";
        case ParseError::MissingBeginString: return "MissingBeginString";
        case ParseError::InvalidBodyLength:  return "InvalidBodyLength";
        case ParseError::ChecksumMismatch:   return "ChecksumMismatch";
        case ParseError::UnexpectedEnd:      return "UnexpectedEnd";
        case ParseError::MalformedTag:       return "MalformedTag";
        case ParseError::MalformedValue:     return "MalformedValue";
        case ParseError::MalformedDataField: return "MalformedDataField";
    }
    return "?";
}

struct Field {
    int              tag;
    std::string_view value;
};

struct ParseResult {
    ParseError error             = ParseError::Ok;
    int        msg_type_char     = 0;
    uint32_t   seq_num           = 0;
    uint8_t    checksum          = 0;
    uint8_t    expected_checksum = 0;
    int        field_count       = 0;
    int64_t    body_length       = -1;   // tag 9 as declared
    int64_t    body_observed     = -1;   // bytes between tag-9 SOH and "10="
};

class Parser {
public:
    // Parse "NNN=" — returns tag or -1. p is advanced past '='.
    [[nodiscard]] static int
    parse_tag(const char*& p, const char* end) noexcept {
        int tag = 0;
        int digits = 0;
        while (p < end) {
            char c = *p++;
            if (__builtin_expect(c == '=', 0)) return digits ? tag : -1;
            if (__builtin_expect(c < '0' || c > '9', 0)) return -1;
            if (__builtin_expect(++digits > 6, 0)) return -1;   // FIX tags < 1e6
            tag = tag * 10 + (c - '0');
        }
        return -1;
    }

    // Scan to next SOH. p is advanced past the SOH. Returns nullopt on
    // unterminated value (caller decides whether that is an error).
    [[nodiscard]] static std::optional<std::string_view>
    parse_value(const char*& p, const char* end) noexcept {
        const char* start = p;
        const char* soh   = simd::find_byte(p, end, kSOH);
        if (__builtin_expect(soh == end, 0)) { p = end; return std::nullopt; }
        p = soh + 1;
        return std::string_view(start, size_t(soh - start));
    }

    // Read exactly `len` bytes then require SOH. Used for data fields.
    [[nodiscard]] static std::optional<std::string_view>
    parse_data(const char*& p, const char* end, int64_t len) noexcept {
        if (len < 0 || end - p < len + 1) return std::nullopt;
        const char* start = p;
        p += len;
        if (*p != kSOH) return std::nullopt;
        ++p;
        return std::string_view(start, size_t(len));
    }

    [[nodiscard]] static std::optional<int64_t>
    parse_int(std::string_view sv) noexcept {
        int64_t val;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
        if (ec != std::errc{} || ptr != sv.data() + sv.size())
            return std::nullopt;
        return val;
    }

    // Full parse. Visitor is called once per field in wire order, including
    // header fields and data fields, excluding CheckSum.
    template<typename Visitor>
    [[nodiscard]] static ParseResult
    parse(std::span<const char> msg, Visitor&& visit) noexcept {
        ParseResult r;
        if (msg.empty()) { r.error = ParseError::EmptyMessage; return r; }

        const char* p          = msg.data();
        const char* end        = p + msg.size();
        const char* body_start = nullptr;
        uint8_t     checksum   = 0;
        int         pending_data_tag = -1;
        int64_t     pending_data_len = 0;
        bool        first = true;
        bool        saw_checksum = false;

        while (p < end) {
            const char* field_start = p;
            int tag = parse_tag(p, end);
            if (__builtin_expect(tag < 0, 0)) { r.error = ParseError::MalformedTag; return r; }

            if (__builtin_expect(first, 0)) {
                first = false;
                if (tag != Tag::BeginString) { r.error = ParseError::MissingBeginString; return r; }
            }

            if (__builtin_expect(tag == Tag::CheckSum, 0)) {
                if (body_start) r.body_observed = field_start - body_start;
                auto v = parse_value(p, end);
                if (!v) { r.error = ParseError::UnexpectedEnd; return r; }
                auto n = parse_int(*v);
                if (!n || v->size() != 3) { r.error = ParseError::MalformedValue; return r; }
                r.expected_checksum = uint8_t(*n);
                saw_checksum = true;
                break;
            }

            std::optional<std::string_view> value;
            if (__builtin_expect(tag == pending_data_tag, 0)) {
                value = parse_data(p, end, pending_data_len);
                if (!value) { r.error = ParseError::MalformedDataField; return r; }
                pending_data_tag = -1;
            } else {
                value = parse_value(p, end);
                if (!value) { r.error = ParseError::UnexpectedEnd; return r; }
            }

            // Checksum covers "tag=value\001" for every field except tag 10.
            checksum += simd::sum_bytes(field_start, p);

            if (tag == Tag::MsgType) {
                if (!value->empty()) r.msg_type_char = int((*value)[0]);
            } else if (tag == Tag::MsgSeqNum) {
                if (auto n = parse_int(*value)) r.seq_num = uint32_t(*n);
            } else if (tag == Tag::BodyLength) {
                auto n = parse_int(*value);
                if (!n) { r.error = ParseError::InvalidBodyLength; return r; }
                r.body_length = *n;
                body_start = p;
            } else if (int dt = data_tag_for_length(tag); dt >= 0) {
                auto n = parse_int(*value);
                if (!n || *n < 0) { r.error = ParseError::MalformedDataField; return r; }
                pending_data_tag = dt;
                pending_data_len = *n;
            }

            visit(Field{tag, *value});
            ++r.field_count;
        }

        r.checksum = checksum;
        if (!saw_checksum) { r.error = ParseError::UnexpectedEnd; return r; }
        if (r.body_length >= 0 && r.body_observed != r.body_length) {
            r.error = ParseError::InvalidBodyLength;
            return r;
        }
        if (r.checksum != r.expected_checksum)
            r.error = ParseError::ChecksumMismatch;
        return r;
    }

    // Fast path: MsgType + MsgSeqNum only. No checksum, no body-length check.
    // Use for sequence-gap detection before committing to a full parse.
    [[nodiscard]] static ParseResult
    parse_fast(std::span<const char> msg) noexcept {
        ParseResult r;
        if (msg.empty()) { r.error = ParseError::EmptyMessage; return r; }

        const char* p   = msg.data();
        const char* end = p + msg.size();
        bool found_type = false, found_seq = false;

        while (p < end) {
            int tag = parse_tag(p, end);
            if (tag < 0) { r.error = ParseError::MalformedTag; return r; }
            auto value = parse_value(p, end);
            if (!value) { r.error = ParseError::UnexpectedEnd; return r; }

            if (tag == Tag::MsgType) {
                if (!value->empty()) r.msg_type_char = int((*value)[0]);
                found_type = true;
            } else if (tag == Tag::MsgSeqNum) {
                if (auto n = parse_int(*value)) r.seq_num = uint32_t(*n);
                found_seq = true;
            } else if (tag == Tag::CheckSum) {
                break;
            }
            if (found_type && found_seq) break;
        }
        return r;
    }
};

// ── NewOrderSingle (MsgType D) ───────────────────────────────────────────────

struct NewOrderSingle {
    std::string_view cl_ord_id;
    std::string_view symbol;
    std::string_view transact_time;
    int64_t          order_qty  = 0;
    int64_t          price      = 0;
    char             side       = 0;
    char             ord_type   = 0;
    char             handl_inst = 0;

    [[nodiscard]] static std::optional<NewOrderSingle>
    from(std::span<const char> msg) noexcept {
        NewOrderSingle order;
        auto result = Parser::parse(msg, [&](fix::Field f) {
            switch (f.tag) {
                case Tag::ClOrdID:      order.cl_ord_id     = f.value; break;
                case Tag::Symbol:       order.symbol        = f.value; break;
                case Tag::TransactTime: order.transact_time = f.value; break;
                case Tag::Side:      if (!f.value.empty()) order.side       = f.value[0]; break;
                case Tag::OrdType:   if (!f.value.empty()) order.ord_type   = f.value[0]; break;
                case Tag::HandlInst: if (!f.value.empty()) order.handl_inst = f.value[0]; break;
                case Tag::OrderQty: { auto n = Parser::parse_int(f.value); if (n) order.order_qty = *n; break; }
                case Tag::Price:    { auto n = Parser::parse_int(f.value); if (n) order.price     = *n; break; }
            }
        });
        if (result.error != ParseError::Ok) return std::nullopt;
        return order;
    }
};

// ── ExecutionReport (MsgType 8) ──────────────────────────────────────────────

struct ExecutionReport {
    std::string_view order_id;
    std::string_view exec_id;
    std::string_view cl_ord_id;
    std::string_view symbol;
    int64_t          leaves_qty = 0;
    int64_t          cum_qty    = 0;
    int64_t          avg_px     = 0;
    char             exec_type  = 0;
    char             ord_status = 0;
    char             side       = 0;

    [[nodiscard]] static std::optional<ExecutionReport>
    from(std::span<const char> msg) noexcept {
        ExecutionReport report;
        auto result = Parser::parse(msg, [&](fix::Field f) {
            switch (f.tag) {
                case Tag::OrderID:   report.order_id  = f.value; break;
                case Tag::ExecID:    report.exec_id   = f.value; break;
                case Tag::ClOrdID:   report.cl_ord_id = f.value; break;
                case Tag::Symbol:    report.symbol    = f.value; break;
                case Tag::ExecType:  if (!f.value.empty()) report.exec_type  = f.value[0]; break;
                case Tag::OrdStatus: if (!f.value.empty()) report.ord_status = f.value[0]; break;
                case Tag::Side:      if (!f.value.empty()) report.side       = f.value[0]; break;
                case Tag::LeavesQty: { auto n = Parser::parse_int(f.value); if (n) report.leaves_qty = *n; break; }
                case Tag::CumQty:    { auto n = Parser::parse_int(f.value); if (n) report.cum_qty    = *n; break; }
                case Tag::AvgPx:     { auto n = Parser::parse_int(f.value); if (n) report.avg_px     = *n; break; }
            }
        });
        if (result.error != ParseError::Ok) return std::nullopt;
        return report;
    }
};

[[nodiscard]] inline uint8_t
compute_checksum(std::span<const char> msg) noexcept {
    return simd::sum_bytes(msg.data(), msg.data() + msg.size());
}

} // namespace fix
