// tests/test_session.cpp — FIX session-layer state machine tests.
//
// Covers: logon, logout, heartbeat, test request, resend request,
// sequence reset, gap detection, duplicate detection, error injection.

#include "fix/session.hpp"
#include "fix/parser.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

static int g_passed = 0, g_failed = 0;

#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++g_failed; } else { ++g_passed; } \
    } while(0)

// ── Test harness ──────────────────────────────────────────────────────────

struct TestSession {
    fix::Session session;
    std::vector<std::string> sent;
    std::vector<std::string> errors;
    std::vector<std::pair<uint64_t,uint64_t>> gaps;
    std::vector<fix::SessionState> state_changes;
    int messages_delivered = 0;

    TestSession() : session(
        fix::SessionConfig{
            .sender_comp_id = "CLIENT",
            .target_comp_id = "SERVER",
        },
        fix::SessionCallbacks{
            .on_message = [this](auto const&, auto) { ++messages_delivered; },
            .on_send = [this](std::string_view msg) { sent.emplace_back(msg); },
            .on_state_change = [this](auto, auto ns) { state_changes.push_back(ns); },
            .on_gap = [this](uint64_t e, uint64_t r) { gaps.push_back({e, r}); },
            .on_reject = nullptr,
            .on_error = [this](std::string_view e) { errors.emplace_back(e); },
        }) {}

    std::string last_sent() const { return sent.empty() ? "" : sent.back(); }

    // Build a minimal valid FIX message for testing.
    static std::string make_msg(std::string_view type, uint64_t seq,
                                std::string_view extra = "") {
        return fix::MessageBuilder::build(
            "FIX.4.2", type, "SERVER", "CLIENT", seq, extra);
    }

    void recv(std::string const& msg) {
        session.on_recv(std::span<char const>(msg.data(), msg.size()));
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────

static void test_logon_initiator() {
    std::printf("  ── Logon (initiator)\n");
    TestSession t;
    t.session.on_connect();
    CHECK(t.session.state() == fix::SessionState::Connected, "connected after on_connect");

    t.session.logon();
    CHECK(t.session.state() == fix::SessionState::LoggingOn, "LoggingOn after logon()");
    CHECK(!t.sent.empty(), "Logon message sent");
    CHECK(t.last_sent().find("35=A") != std::string::npos, "Logon has MsgType=A");

    // Receive Logon response from server.
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));
    CHECK(t.session.state() == fix::SessionState::LoggedOn, "LoggedOn after Logon response");
    CHECK(t.session.incoming_seq() == 2, "Sequence advanced to 2");
}

static void test_logon_acceptor() {
    std::printf("  ── Logon (acceptor — server initiates)\n");
    TestSession t;
    t.session.on_connect();

    // Server sends Logon first.
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));
    CHECK(t.session.state() == fix::SessionState::LoggedOn, "LoggedOn on recv Logon");
    // Acceptor should send Logon response.
    CHECK(!t.sent.empty(), "Logon response sent");
}

static void test_logout() {
    std::printf("  ── Logout\n");
    TestSession t;
    t.session.on_connect();
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));

    t.session.logout("end of day");
    CHECK(t.session.state() == fix::SessionState::LoggingOut, "LoggingOut after logout()");
    CHECK(t.last_sent().find("35=5") != std::string::npos, "Logout message sent");

    // Server acknowledges.
    t.recv(TestSession::make_msg("5", 2));
    CHECK(t.session.state() == fix::SessionState::NotConnected, "NotConnected after Logout ack");
}

static void test_peer_initiates_logout() {
    std::printf("  ── Peer-initiated logout\n");
    TestSession t;
    t.session.on_connect();
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));

    // Server sends Logout.
    t.recv(TestSession::make_msg("5", 2));
    CHECK(t.session.state() == fix::SessionState::NotConnected, "NotConnected on peer Logout");
    // We should have sent a Logout response.
    bool found_logout = false;
    for (auto const& s : t.sent)
        if (s.find("35=5") != std::string::npos) found_logout = true;
    CHECK(found_logout, "Logout response sent to peer");
}

static void test_heartbeat() {
    std::printf("  ── Heartbeat\n");
    TestSession t;
    t.session.on_connect();
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));

    // Receive a Heartbeat.
    size_t before = t.sent.size();
    t.recv(TestSession::make_msg("0", 2));
    CHECK(t.session.state() == fix::SessionState::LoggedOn, "still logged on after heartbeat");
    CHECK(t.session.incoming_seq() == 3, "sequence advanced");
    (void)before;
}

static void test_test_request() {
    std::printf("  ── Test Request / Heartbeat response\n");
    TestSession t;
    t.session.on_connect();
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));

    // Server sends Test Request.
    t.recv(TestSession::make_msg("1", 2, "112=PROBE\x01"));
    // We should have sent a Heartbeat in response with TestReqID.
    bool found_hb = false;
    for (auto const& s : t.sent)
        if (s.find("35=0") != std::string::npos &&
            s.find("112=PROBE") != std::string::npos) found_hb = true;
    CHECK(found_hb, "Heartbeat response with TestReqID sent");
}

static void test_sequence_gap_detection() {
    std::printf("  ── Sequence gap detection\n");
    TestSession t;
    t.session.on_connect();
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));

    // Expected seq=2 but receive seq=5 — gap.
    t.recv(TestSession::make_msg("0", 5));
    CHECK(!t.gaps.empty(), "gap detected");
    CHECK(t.gaps[0].first == 2, "expected seq=2");
    CHECK(t.gaps[0].second == 5, "received seq=5");

    // Resend Request should have been sent.
    bool found_rr = false;
    for (auto const& s : t.sent)
        if (s.find("35=2") != std::string::npos) found_rr = true;
    CHECK(found_rr, "Resend Request sent on gap");
}

static void test_duplicate_detection() {
    std::printf("  ── Duplicate message detection\n");
    TestSession t;
    t.session.on_connect();
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));
    t.recv(TestSession::make_msg("0", 2));  // seq=2, expected

    // Send seq=1 again — duplicate.
    size_t errors_before = t.errors.size();
    size_t sent_before = t.sent.size();
    t.recv(TestSession::make_msg("0", 1));  // duplicate

    // Should have sent a Reject or just ignored.
    bool sent_reject = false;
    for (size_t i = sent_before; i < t.sent.size(); ++i)
        if (t.sent[i].find("35=3") != std::string::npos) sent_reject = true;
    CHECK(sent_reject || t.errors.size() > errors_before,
          "duplicate rejected or error raised");
    CHECK(t.session.incoming_seq() == 3, "sequence not advanced on duplicate");
}

static void test_sequence_reset() {
    std::printf("  ── Sequence Reset\n");
    TestSession t;
    t.session.on_connect();
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));

    // Server sends Sequence Reset to seq 100.
    std::string body = "123=Y\x01"; body += "36=100\x01";
    t.recv(TestSession::make_msg("4", 2, body));
    CHECK(t.session.incoming_seq() == 100, "incoming seq reset to 100");
}

static void test_checksum_error_injection() {
    std::printf("  ── Checksum error injection\n");
    TestSession t;
    t.session.on_connect();
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));

    // Corrupt a message.
    std::string msg = TestSession::make_msg("0", 2);
    // Flip a byte in the body to corrupt checksum.
    if (msg.size() > 20) msg[20] ^= 0xFF;

    size_t errors_before = t.errors.size();
    t.recv(msg);
    CHECK(t.errors.size() > errors_before || t.session.incoming_seq() == 2,
          "corrupted message rejected");
}

static void test_application_message_delivery() {
    std::printf("  ── Application message delivery\n");
    TestSession t;
    t.session.on_connect();
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));

    // Send a NewOrderSingle (type D).
    t.recv(TestSession::make_msg("D", 2, "11=ORD001\x01""55=AAPL\x01""54=1\x01"));
    CHECK(t.messages_delivered == 1, "application message delivered to callback");
    CHECK(t.session.incoming_seq() == 3, "sequence advanced");
}

static void test_state_transitions() {
    std::printf("  ── State transition sequence\n");
    TestSession t;
    t.session.on_connect();
    t.session.logon();
    t.recv(TestSession::make_msg("A", 1, "108=30\x01"));
    t.session.logout();
    t.recv(TestSession::make_msg("5", 2));

    // Expected transitions: Connected → LoggingOn → LoggedOn → LoggingOut → NotConnected
    CHECK(t.state_changes.size() >= 4, "at least 4 state transitions");
    CHECK(t.state_changes[0] == fix::SessionState::Connected, "Connected");
    CHECK(t.state_changes[1] == fix::SessionState::LoggingOn, "LoggingOn");
    CHECK(t.state_changes[2] == fix::SessionState::LoggedOn, "LoggedOn");
    CHECK(t.state_changes[3] == fix::SessionState::LoggingOut, "LoggingOut");
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("FIX Session Layer Tests\n");
    std::printf("═══════════════════════\n\n");

    test_logon_initiator();
    test_logon_acceptor();
    test_logout();
    test_peer_initiates_logout();
    test_heartbeat();
    test_test_request();
    test_sequence_gap_detection();
    test_duplicate_detection();
    test_sequence_reset();
    test_checksum_error_injection();
    test_application_message_delivery();
    test_state_transitions();

    std::printf("\n═══════════════════════\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
