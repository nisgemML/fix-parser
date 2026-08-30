// tests/test_parser.cpp — Correctness tests for the FIX 4.2/4.4 parser.
#include "test_util.hpp"
#include <random>
#include <vector>

using fix::Parser; using fix::ParseError; using fix::Field;

static void test_new_order_single_basic() {
    printf("NewOrderSingle basic parse:\n");
    auto full = build(soh("35=D|49=SENDER|56=TARGET|34=1|52=20230901-09:30:00.000|"
                          "11=CLORD001|21=1|55=AAPL|54=1|60=20230901-09:30:00|38=100|40=2|44=15025|"));
    auto order = fix::NewOrderSingle::from(sp(full));
    CHECK(order.has_value(),              "NewOrderSingle parsed");
    CHECK(order->cl_ord_id == "CLORD001", "ClOrdID correct");
    CHECK(order->symbol    == "AAPL",     "Symbol correct");
    CHECK(order->side      == '1',        "Side=Buy");
    CHECK(order->ord_type  == '2',        "OrdType=Limit");
    CHECK(order->order_qty == 100,        "OrderQty correct");
    CHECK(order->price     == 15025,      "Price correct");
}

static void test_execution_report_fill() {
    printf("ExecutionReport fill parse:\n");
    auto full = build(soh("35=8|49=TARGET|56=SENDER|34=2|52=20230901-09:30:00.100|"
                          "37=ORD001|17=EXEC001|11=CLORD001|55=AAPL|54=1|150=2|39=2|151=0|14=100|6=15025|"));
    auto r = fix::ExecutionReport::from(sp(full));
    CHECK(r.has_value(),             "ExecutionReport parsed");
    CHECK(r->order_id  == "ORD001",  "OrderID correct");
    CHECK(r->exec_id   == "EXEC001", "ExecID correct");
    CHECK(r->exec_type == '2',       "ExecType=Filled");
    CHECK(r->leaves_qty == 0,        "LeavesQty=0");
    CHECK(r->cum_qty    == 100,      "CumQty=100");
    CHECK(r->avg_px     == 15025,    "AvgPx=15025");
}

static void test_checksum_and_bodylength() {
    printf("Checksum + BodyLength verification:\n");
    auto full = build(soh("35=0|49=SENDER|56=TARGET|34=1|52=20230901-09:30:00|"));
    auto res = Parser::parse(sp(full), [](Field){});
    CHECK(res.error == ParseError::Ok, "valid message accepted");
    CHECK(res.body_length == res.body_observed, "body length reconciles");

    std::string corrupt = full; corrupt[20] ^= 0x01;
    res = Parser::parse(sp(corrupt), [](Field){});
    CHECK(res.error == ParseError::ChecksumMismatch, "corrupt byte → ChecksumMismatch");

    // Lie about BodyLength: declared 5 bytes short. Checksum is recomputed
    // so only the length check can catch it.
    std::string body = soh("35=0|49=A|56=B|34=1|");
    std::string lie = "8=FIX.4.4\x01" "9=" + std::to_string(body.size() - 5) + "\x01" + body;
    char buf[16]; snprintf(buf, sizeof buf, "10=%03d\x01", int(fix::compute_checksum(sp(lie))));
    lie += buf;
    res = Parser::parse(sp(lie), [](Field){});
    CHECK(res.error == ParseError::InvalidBodyLength, "wrong BodyLength rejected");

    // Truncated before tag 10.
    std::string cut = full.substr(0, full.size() - 7);
    res = Parser::parse(sp(cut), [](Field){});
    CHECK(res.error == ParseError::UnexpectedEnd, "missing CheckSum → UnexpectedEnd");

    // Does not start with tag 8.
    auto bad = soh("35=0|8=FIX.4.4|10=000|");
    res = Parser::parse(sp(bad), [](Field){});
    CHECK(res.error == ParseError::MissingBeginString, "first tag must be 8");
}

static void test_data_field_with_embedded_soh() {
    printf("Length-prefixed data fields (SOH inside RawData):\n");
    // RawData contains SOH, '=', and a fake "10=000" — all must be opaque.
    std::string raw = "\x01" "10=000" "\x01" "35=D" "\x01";
    std::string body = soh("35=0|49=A|56=B|34=7|") + "95=" + std::to_string(raw.size()) +
                       "\x01" + "96=" + raw + "\x01" + soh("108=30|");
    auto full = build(body);
    std::string_view seen_raw; int seen_108 = 0; int count = 0;
    auto res = Parser::parse(sp(full), [&](Field f) {
        ++count;
        if (f.tag == 96)  seen_raw = f.value;
        if (f.tag == 108) seen_108 = 1;
    });
    CHECK(res.error == ParseError::Ok,        "message with binary RawData parses");
    CHECK(seen_raw == raw,                    "RawData returned verbatim including SOH");
    CHECK(seen_108 == 1,                      "field after RawData still visited");
    CHECK(res.seq_num == 7,                   "SeqNum correct");
    CHECK(count == 9, "all 9 non-checksum fields visited");

    // Declared length longer than the wire → MalformedDataField, never OOB.
    std::string bad_body = soh("35=0|49=A|56=B|34=1|95=999|96=abc|");
    auto bad = build(bad_body);
    res = Parser::parse(sp(bad), [](Field){});
    CHECK(res.error == ParseError::MalformedDataField, "over-long data length rejected");

    // Negative length.
    auto neg = build(soh("35=0|49=A|56=B|34=1|95=-1|96=abc|"));
    res = Parser::parse(sp(neg), [](Field){});
    CHECK(res.error == ParseError::MalformedDataField, "negative data length rejected");
}

static void test_parse_fast() {
    printf("parse_fast (MsgType + SeqNum only):\n");
    auto full = build(soh("35=D|49=SENDER|56=TARGET|34=42|52=20230901-09:30:00|11=X|55=AAPL|54=1|38=100|40=2|44=15025|"));
    auto res = Parser::parse_fast(sp(full));
    CHECK(res.msg_type_char == int('D'), "MsgType=D");
    CHECK(res.seq_num == 42,             "SeqNum=42");
    const char* types[] = {"D","8","F","0","A","5"};
    for (auto t : types) {
        auto m = build(std::string("35=") + t + soh("|49=A|56=B|34=1|"));
        CHECK(Parser::parse_fast(sp(m)).msg_type_char == int(t[0]), "MsgType round-trip");
    }
}

static void test_malformed_tags() {
    printf("Malformed tags:\n");
    auto r1 = Parser::parse(sp(soh("8=FIX.4.4|9=5|=abc|10=000|")), [](Field){});
    CHECK(r1.error == ParseError::MalformedTag, "empty tag rejected");
    auto r2 = Parser::parse(sp(soh("8=FIX.4.4|9=5|1234567=x|10=000|")), [](Field){});
    CHECK(r2.error == ParseError::MalformedTag, "7-digit tag rejected");
    auto r3 = Parser::parse(sp(soh("8=FIX.4.4|9=5|3a=x|10=000|")), [](Field){});
    CHECK(r3.error == ParseError::MalformedTag, "non-digit in tag rejected");
    auto r4 = Parser::parse(sp(std::string("8=FIX.4.4\x01" "9=5\x01" "35=D")), [](Field){});
    CHECK(r4.error == ParseError::UnexpectedEnd, "unterminated value rejected");
    auto e = Parser::parse({}, [](Field){});
    CHECK(e.error == ParseError::EmptyMessage, "empty message error");
}

static void test_long_values_cross_simd_boundary() {
    printf("Values longer than one SIMD lane (32B) and 64B:\n");
    std::string v33(33, 'x'), v64(64, 'y'), v100(100, 'z');
    auto full = build(soh("35=D|49=A|56=B|34=1|11=") + v33 + "\x01" "58=" + v64 + "\x01" "55=" + v100 + "\x01");
    std::string_view a, b, c;
    auto res = Parser::parse(sp(full), [&](Field f) {
        if (f.tag == 11) a = f.value;
        if (f.tag == 58) b = f.value;
        if (f.tag == 55) c = f.value;
    });
    CHECK(res.error == ParseError::Ok, "long values parse");
    CHECK(a == v33 && b == v64 && c == v100, "long values exact");
}

static void test_simd_scalar_equivalence() {
    printf("SIMD vs scalar equivalence on random buffers:\n");
    std::mt19937_64 rng(42);
    int mismatches = 0;
    for (int iter = 0; iter < 5000; ++iter) {
        size_t n = rng() % 300;
        std::vector<char> buf(n);
        for (auto& ch : buf) ch = char(rng() % 256);
        const char* b = buf.data(); const char* e = b + n;
        for (char needle : {'\x01', '=', '\0', char(0xff)}) {
            if (fix::simd::find_byte(b, e, needle) != fix::simd::find_byte_scalar(b, e, needle)) ++mismatches;
        }
        if (fix::simd::sum_bytes(b, e) != fix::simd::sum_bytes_scalar(b, e)) ++mismatches;
        // sub-slices at odd offsets
        if (n > 5) {
            const char* b2 = b + 3;
            if (fix::simd::sum_bytes(b2, e) != fix::simd::sum_bytes_scalar(b2, e)) ++mismatches;
        }
    }
    CHECK(mismatches == 0, "find_byte and sum_bytes agree with scalar on 5000 random buffers");
    printf("  AVX2 path compiled: %s\n", FIX_HAVE_AVX2 ? "yes" : "no");
}

static void test_zero_copy() {
    printf("Zero-copy: string_view into original buffer:\n");
    auto full = build(soh("35=D|49=A|56=B|34=1|11=ORDER_ID_123|55=GOOGL|54=1|38=50|40=2|44=14000|"));
    auto order = fix::NewOrderSingle::from(sp(full));
    CHECK(order.has_value(), "parsed");
    auto inside = [&](std::string_view v) {
        return v.data() >= full.data() && v.data() < full.data() + full.size(); };
    CHECK(inside(order->cl_ord_id) && inside(order->symbol), "fields point into original buffer");
}

static void test_large_ints() {
    printf("Large integer parsing:\n");
    auto full = build(soh("35=D|49=A|56=B|34=999999|38=1000000|44=9999999|11=X|55=Y|54=1|40=2|"));
    auto o = fix::NewOrderSingle::from(sp(full));
    CHECK(o && o->order_qty == 1000000 && o->price == 9999999, "large ints");
    CHECK(!Parser::parse_int("12a"), "trailing garbage rejected");
    CHECK(!Parser::parse_int(""),    "empty int rejected");
}

int main() {
    printf("=== FIX 4.2/4.4 Parser Tests ===\n\n");
    test_new_order_single_basic();
    test_execution_report_fill();
    test_checksum_and_bodylength();
    test_data_field_with_embedded_soh();
    test_parse_fast();
    test_malformed_tags();
    test_long_values_cross_simd_boundary();
    test_simd_scalar_equivalence();
    test_zero_copy();
    test_large_ints();
    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
