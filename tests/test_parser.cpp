// tests/test_parser.cpp — Correctness tests for FIX 4.2/4.4 parser.

#include "fix/parser.hpp"
#include <cstdio>
#include <string>

static int passed = 0, failed = 0;

#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; \
    } else { ++passed; } } while(0)

static std::string fix_msg(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p)
        out += (*p == '|') ? '\x01' : *p;
    return out;
}

static std::string checksum_field(const std::string& body) {
    uint8_t cs = fix::compute_checksum({body.data(), body.size()});
    char buf[16];
    snprintf(buf, sizeof(buf), "10=%03d\x01", int(cs));
    return buf;
}

void test_new_order_single_basic() {
    printf("NewOrderSingle basic parse:\n");
    std::string header = fix_msg("8=FIX.4.2|9=178|35=D|49=SENDER|56=TARGET|34=1|52=20230901-09:30:00.000|");
    std::string body   = fix_msg("11=CLORD001|21=1|55=AAPL|54=1|60=20230901-09:30:00|38=100|40=2|44=15025|");
    std::string full   = header + body + checksum_field(header + body);
    auto span  = std::span<const char>(full.data(), full.size());
    auto order = fix::NewOrderSingle::from(span);
    CHECK(order.has_value(),          "NewOrderSingle parsed");
    CHECK(order->cl_ord_id == "CLORD001", "ClOrdID correct");
    CHECK(order->symbol    == "AAPL",     "Symbol correct");
    CHECK(order->side      == '1',        "Side=Buy");
    CHECK(order->ord_type  == '2',        "OrdType=Limit");
    CHECK(order->order_qty == 100,        "OrderQty correct");
    CHECK(order->price     == 15025,      "Price correct");
}

void test_execution_report_fill() {
    printf("ExecutionReport fill parse:\n");
    std::string header = fix_msg("8=FIX.4.2|9=200|35=8|49=TARGET|56=SENDER|34=2|52=20230901-09:30:00.100|");
    std::string body   = fix_msg("37=ORD001|17=EXEC001|11=CLORD001|55=AAPL|54=1|150=2|39=2|151=0|14=100|6=15025|");
    std::string full   = header + body + checksum_field(header + body);
    auto span   = std::span<const char>(full.data(), full.size());
    auto report = fix::ExecutionReport::from(span);
    CHECK(report.has_value(),              "ExecutionReport parsed");
    CHECK(report->order_id  == "ORD001",   "OrderID correct");
    CHECK(report->exec_id   == "EXEC001",  "ExecID correct");
    CHECK(report->exec_type == '2',        "ExecType=Filled");
    CHECK(report->leaves_qty == 0,         "LeavesQty=0");
    CHECK(report->cum_qty    == 100,       "CumQty=100");
    CHECK(report->avg_px     == 15025,     "AvgPx=15025");
}

void test_checksum_verification() {
    printf("Checksum verification:\n");
    std::string header = fix_msg("8=FIX.4.2|9=50|35=0|49=SENDER|56=TARGET|34=1|52=20230901-09:30:00|");
    std::string full   = header + checksum_field(header);
    auto span   = std::span<const char>(full.data(), full.size());
    auto result = fix::Parser::parse(span, [](fix::Field){});
    CHECK(result.error == fix::ParseError::Ok, "valid checksum accepted");

    std::string corrupt = full;
    corrupt[10] ^= 0x01;
    auto corrupt_span = std::span<const char>(corrupt.data(), corrupt.size());
    result = fix::Parser::parse(corrupt_span, [](fix::Field){});
    CHECK(result.error == fix::ParseError::ChecksumMismatch, "corrupt checksum rejected");
}

void test_parse_fast_msgtype_seqnum() {
    printf("parse_fast (MsgType + SeqNum only):\n");
    std::string header = fix_msg("8=FIX.4.2|9=178|35=D|49=SENDER|56=TARGET|34=42|52=20230901-09:30:00|");
    std::string body   = fix_msg("11=CLORD001|55=AAPL|54=1|38=100|40=2|44=15025|");
    std::string full   = header + body + checksum_field(header + body);
    auto span   = std::span<const char>(full.data(), full.size());
    auto result = fix::Parser::parse_fast(span);
    CHECK(result.msg_type_char == int('D'), "MsgType=D");
    CHECK(result.seq_num == 42,             "SeqNum=42");
}

void test_field_iteration() {
    printf("Field iteration:\n");
    std::string header = fix_msg("8=FIX.4.2|9=100|35=A|49=SENDER|56=TARGET|34=1|52=20230901-09:30:00|");
    std::string body   = fix_msg("98=0|108=30|");
    std::string full   = header + body + checksum_field(header + body);
    int tag_count = 0; bool found_35 = false;
    auto span = std::span<const char>(full.data(), full.size());
    fix::Parser::parse(span, [&](fix::Field f) {
        ++tag_count;
        if (f.tag == 35 && f.value == "A") found_35 = true;
    });
    CHECK(tag_count >= 8, "all fields visited");
    CHECK(found_35,       "tag 35 found with value A");
}

void test_side_values() {
    printf("Side field parsing:\n");
    auto make_order = [](char side_char) {
        std::string body = std::string("8=FIX.4.2\x01""9=100\x01""35=D\x01"
            "49=A\x01""56=B\x01""34=1\x01""52=20230901-09:30:00\x01"
            "11=C\x01""55=AAPL\x01""54=");
        body += side_char;
        body += "\x01""38=100\x01""40=2\x01""44=100\x01";
        body += checksum_field(body);
        return body;
    };
    auto buy  = make_order('1');
    auto sell = make_order('2');
    auto buy_order  = fix::NewOrderSingle::from({buy.data(),  buy.size()});
    auto sell_order = fix::NewOrderSingle::from({sell.data(), sell.size()});
    CHECK(buy_order.has_value()  && buy_order->side  == '1', "Buy side=1");
    CHECK(sell_order.has_value() && sell_order->side == '2', "Sell side=2");
}

void test_empty_message() {
    printf("Edge cases:\n");
    auto empty = fix::Parser::parse({}, [](fix::Field){});
    CHECK(empty.error == fix::ParseError::EmptyMessage, "empty message error");
}

void test_multiple_message_types() {
    printf("Multiple message types:\n");
    const char* types[] = {"D","8","F","0","A","5"};
    const char* names[] = {"NewOrderSingle","ExecutionReport","OrderCancel",
                           "Heartbeat","Logon","Logout"};
    for (int i = 0; i < 6; ++i) {
        std::string msg = std::string("8=FIX.4.2\x01""9=50\x01""35=") +
                          types[i] + "\x01""49=A\x01""56=B\x01""34=1\x01";
        msg += checksum_field(msg);
        auto result = fix::Parser::parse_fast({msg.data(), msg.size()});
        CHECK(result.msg_type_char == int(types[i][0]),
              (std::string("MsgType ") + names[i]).c_str());
    }
}

void test_large_quantities() {
    printf("Large integer parsing:\n");
    std::string body = std::string("8=FIX.4.2\x01""9=100\x01""35=D\x01"
        "49=A\x01""56=B\x01""34=999999\x01""38=1000000\x01""44=9999999\x01"
        "11=X\x01""55=Y\x01""54=1\x01""40=2\x01");
    body += checksum_field(body);
    auto order = fix::NewOrderSingle::from({body.data(), body.size()});
    CHECK(order.has_value() && order->order_qty == 1000000, "large OrderQty");
    CHECK(order.has_value() && order->price     == 9999999, "large Price");
}

void test_string_view_zero_copy() {
    printf("Zero-copy: string_view into original buffer:\n");
    std::string body = std::string("8=FIX.4.2\x01""9=100\x01""35=D\x01"
        "49=A\x01""56=B\x01""34=1\x01""11=ORDER_ID_123\x01"
        "55=GOOGL\x01""54=1\x01""38=50\x01""40=2\x01""44=14000\x01");
    body += checksum_field(body);
    auto order = fix::NewOrderSingle::from({body.data(), body.size()});
    CHECK(order.has_value(), "parsed");
    const char* cl_ord_start = order->cl_ord_id.data();
    CHECK(cl_ord_start >= body.data() && cl_ord_start < body.data() + body.size(),
          "cl_ord_id points into original buffer (zero copy)");
    const char* sym_start = order->symbol.data();
    CHECK(sym_start >= body.data() && sym_start < body.data() + body.size(),
          "symbol points into original buffer (zero copy)");
}

int main() {
    printf("=== FIX 4.2/4.4 Parser Tests ===\n\n");
    test_new_order_single_basic();
    test_execution_report_fill();
    test_checksum_verification();
    test_parse_fast_msgtype_seqnum();
    test_field_iteration();
    test_side_values();
    test_empty_message();
    test_multiple_message_types();
    test_large_quantities();
    test_string_view_zero_copy();
    printf("\n================================\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
