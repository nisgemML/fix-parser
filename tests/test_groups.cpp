// tests/test_groups.cpp — Repeating-group parsing against the generated FIX 4.4 dictionary.
#include "test_util.hpp"
#include "fix/groups.hpp"
#include "fix/dictionary_fix44.hpp"
#include <vector>

using namespace fix;
static const Dictionary& D = fix44::kDictionary;

static void test_flat_parties_group() {
    printf("NoPartyIDs (453) with two entries:\n");
    auto full = build(soh("35=D|49=A|56=B|34=1|11=X|55=AAPL|54=1|38=100|40=2|"
                          "453=2|448=TRADER1|447=D|452=11|448=FIRM9|447=D|452=1|"
                          "60=20230901-09:30:00|"));
    auto m = Message<>::from(sp(full), D);
    CHECK(m.result.parse.error == ParseError::Ok, "wire parse ok");
    CHECK(m.result.group_error == GroupError::Ok, "group parse ok");
    CHECK(m.group_size(453) == 2,                   "two party entries");
    CHECK(m.get(453, 0, 448) == "TRADER1",          "entry 0 PartyID");
    CHECK(m.get(453, 0, 452) == "11",               "entry 0 PartyRole");
    CHECK(m.get(453, 1, 448) == "FIRM9",            "entry 1 PartyID");
    CHECK(m.get(453, 1, 452) == "1",                "entry 1 PartyRole");
    CHECK(m.get(60) == "20230901-09:30:00",         "field after group is top-level again");
    CHECK(m.get(55) == "AAPL",                      "field before group is top-level");
    CHECK(!m.get(448),                              "group member not visible at top level");
}

static void test_nested_legs_with_nested_parties() {
    printf("NoLegs (555) → NoNestedPartyIDs (539), two legs, second has nested parties:\n");
    // NewOrderMultileg (AB). Leg 0: plain. Leg 1: two nested parties.
    auto full = build(soh("35=AB|49=A|56=B|34=1|11=ML1|55=SPY|54=1|38=10|40=2|"
                          "555=2|"
                          "600=SPY|624=1|623=1|"
                          "600=SPY|624=2|623=1|539=2|524=NP1|525=D|538=1|524=NP2|525=D|538=2|"
                          "60=20230901-09:30:00|"));
    std::vector<std::pair<int, GroupPath>> seen;
    auto r = GroupParser::parse(sp(full), D, [&](Field f, const GroupPath& p) { seen.emplace_back(f.tag, p); });
    CHECK(r.parse.error == ParseError::Ok && r.group_error == GroupError::Ok, "parses clean");

    auto path_of = [&](int tag, int nth) -> GroupPath {
        int k = 0;
        for (auto& [t, p] : seen) if (t == tag && k++ == nth) return p;
        return GroupPath{};
    };
    // 600 (LegSymbol) first occurrence → {555/0}; second → {555/1}
    GroupPath leg0; leg0.depth = 1; leg0.elems[0] = {555, 0};
    GroupPath leg1; leg1.depth = 1; leg1.elems[0] = {555, 1};
    CHECK(path_of(600, 0) == leg0, "first LegSymbol in leg 0");
    CHECK(path_of(600, 1) == leg1, "second LegSymbol in leg 1");

    // 524 (NestedPartyID) → {555/1, 539/0} then {555/1, 539/1}
    GroupPath np0 = leg1; np0.depth = 2; np0.elems[1] = {539, 0};
    GroupPath np1 = leg1; np1.depth = 2; np1.elems[1] = {539, 1};
    CHECK(path_of(524, 0) == np0, "first NestedPartyID at depth 2, entry 0");
    CHECK(path_of(524, 1) == np1, "second NestedPartyID at depth 2, entry 1");
    CHECK(path_of(538, 1) == np1, "NestedPartyRole follows its NestedPartyID");

    // 539 itself lives inside leg 1
    CHECK(path_of(539, 0) == leg1, "nested count tag is attributed to enclosing leg");

    // 60 after everything is back at depth 0
    CHECK(path_of(60, 0).depth == 0, "TransactTime after nested groups is top-level");
}

static void test_market_data_incremental() {
    printf("MarketDataIncrementalRefresh (X) with three MDEntries:\n");
    auto full = build(soh("35=X|49=A|56=B|34=1|262=REQ1|"
                          "268=3|"
                          "279=0|269=0|55=AAPL|270=150.25|271=100|"
                          "279=0|269=1|55=AAPL|270=150.27|271=200|"
                          "279=2|269=2|55=AAPL|270=150.26|271=50|"));
    auto m = Message<>::from(sp(full), D);
    CHECK(m.result.group_error == GroupError::Ok, "group parse ok");
    CHECK(m.group_size(268) == 3,               "three entries");
    CHECK(m.get(268, 2, 270) == "150.26",       "entry 2 price");
    CHECK(m.get(268, 1, 271) == "200",          "entry 1 size");
    CHECK(m.get(262) == "REQ1",                 "MDReqID top-level");
}

static void test_count_mismatch_and_zero() {
    printf("Count mismatch, zero-count, malformed count:\n");
    auto bad = build(soh("35=D|49=A|56=B|34=1|453=3|448=A|447=D|452=1|448=B|447=D|452=2|60=T|"));
    auto r = GroupParser::parse(sp(bad), D, [](Field, const GroupPath&){});
    CHECK(r.group_error == GroupError::CountMismatch && r.offending_tag == 453, "declared 3, saw 2 → CountMismatch");

    auto zero = build(soh("35=D|49=A|56=B|34=1|453=0|60=T|"));
    auto m = Message<>::from(sp(zero), D);
    CHECK(m.result.group_error == GroupError::Ok, "NoPartyIDs=0 is legal");
    CHECK(m.get(60) == "T",                       "next field is top-level");

    auto mal = build(soh("35=D|49=A|56=B|34=1|453=x|448=A|"));
    r = GroupParser::parse(sp(mal), D, [](Field, const GroupPath&){});
    CHECK(r.group_error == GroupError::MalformedCount, "non-integer count rejected");

    auto orphan = build(soh("35=D|49=A|56=B|34=1|453=1|452=1|448=A|"));
    r = GroupParser::parse(sp(orphan), D, [](Field, const GroupPath&){});
    CHECK(r.group_error == GroupError::OrphanTag && r.offending_tag == 452, "member before delimiter → OrphanTag");
}

static void test_dictionary_sanity() {
    printf("Generated dictionary:\n");
    CHECK(D.n_groups >= 50,                    "FIX44 has >=50 distinct group count tags");
    auto* g = D.find(453);
    CHECK(g && g->is_delimiter(448),           "NoPartyIDs delimiter is PartyID");
    CHECK(g && g->has_member(452),             "PartyRole is a member");
    CHECK(g && !g->has_member(55),             "Symbol is not a member");
    auto* md = D.find(268);
    CHECK(md && md->is_delimiter(269) && md->is_delimiter(279), "NoMDEntries has both W and X delimiters");
    auto* legs = D.find(555);
    CHECK(legs && legs->has_member(539),       "NoLegs contains nested NoNestedPartyIDs count tag");
    for (size_t i = 1; i < D.n_groups; ++i)
        CHECK(D.groups[i-1].count_tag < D.groups[i].count_tag, "table sorted by count tag");
}

int main() {
    printf("=== Repeating Group Tests ===\n\n");
    test_dictionary_sanity();
    test_flat_parties_group();
    test_nested_legs_with_nested_parties();
    test_market_data_incremental();
    test_count_mismatch_and_zero();
    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
