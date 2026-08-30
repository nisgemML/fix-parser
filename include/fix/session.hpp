#pragma once
// include/fix/session.hpp — FIX session-layer state machine (FIXT 1.1 / FIX 4.2/4.4)
//
// ═══════════════════════════════════════════════════════════════════════════
// FIX SESSION PROTOCOL
// ═══════════════════════════════════════════════════════════════════════════
//
// The FIX session layer sits between raw TCP and the application layer.
// It manages:
//
//   1. Logon (tag 35=A)  — establish session, agree heartbeat interval
//   2. Logout (tag 35=5) — graceful session teardown
//   3. Heartbeat (tag 35=0) — keep-alive when no other traffic
//   4. Test Request (tag 35=1) — probe if counterparty is alive
//   5. Resend Request (tag 35=2) — request replay of missed messages
//   6. Sequence Reset (tag 35=4) — reset sequence numbers
//   7. Reject (tag 35=3) — signal message parsing failure
//
// ── Sequence number management ────────────────────────────────────────────
//
// Every FIX message has a tag 34 (MsgSeqNum). The session layer:
//   - Tracks expected incoming seq number
//   - Rejects messages with wrong seq (too high → gap, too low → duplicate)
//   - Sends Resend Request on gap detection
//   - Resets sequences on Sequence Reset message
//
// ── State machine ─────────────────────────────────────────────────────────
//
//   NOT_CONNECTED → (connect) → CONNECTED
//   CONNECTED → (send/recv Logon) → LOGGED_ON
//   LOGGED_ON → (send Logout) → LOGGING_OUT
//   LOGGING_OUT → (recv Logout) → NOT_CONNECTED
//   LOGGED_ON → (heartbeat timeout) → send Test Request → wait for Heartbeat
//   LOGGED_ON → (recv Logout from peer) → send Logout → NOT_CONNECTED
//
// ═══════════════════════════════════════════════════════════════════════════

#include "fix/parser.hpp"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fix {

// ── Session state ─────────────────────────────────────────────────────────
enum class SessionState : uint8_t {
    NotConnected,   // TCP not connected
    Connected,      // TCP connected, no logon yet
    LoggingOn,      // Logon sent, waiting for response
    LoggedOn,       // Session active
    LoggingOut,     // Logout sent, waiting for response
};

// ── Session events (callbacks) ────────────────────────────────────────────
struct SessionCallbacks {
    // Called when a well-formed application message arrives (after session processing).
    std::function<void(ParseResult const&, std::span<char const>)> on_message;

    // Called when session needs to send a message.
    std::function<void(std::string_view)> on_send;

    // Called on session state change.
    std::function<void(SessionState old_state, SessionState new_state)> on_state_change;

    // Called on sequence gap detection: (expected, received).
    std::function<void(uint64_t expected, uint64_t received)> on_gap;

    // Called on reject: (rejected_seq, reason).
    std::function<void(uint64_t rejected_seq, std::string_view reason)> on_reject;

    // Called on error (malformed message, checksum failure, etc.).
    std::function<void(std::string_view error)> on_error;
};

// ── Session configuration ─────────────────────────────────────────────────
struct SessionConfig {
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string begin_string = "FIX.4.2";
    int         heartbeat_interval_s = 30;
    bool        reset_on_logon = false;
};

// ── FIX message builder (minimal — for session messages only) ─────────────
class MessageBuilder {
public:
    static std::string build(
        std::string_view begin_string,
        std::string_view msg_type,
        std::string_view sender,
        std::string_view target,
        uint64_t         seq_num,
        std::string_view body_fields)  // pre-formatted "tag=value\x01..." string
    {
        // Build body (everything between BeginString and Checksum).
        std::string body;
        body += "35="; body += msg_type; body += '\x01';
        body += "49="; body += sender;   body += '\x01';
        body += "56="; body += target;   body += '\x01';
        body += "34="; body += std::to_string(seq_num); body += '\x01';
        // Sending time (simplified — omit for session messages in tests).
        if (!body_fields.empty()) {
            body += body_fields;
        }

        // Full message: 8= + 9= + body + 10=.
        std::string msg;
        msg += "8="; msg += begin_string; msg += '\x01';
        msg += "9="; msg += std::to_string(body.size()); msg += '\x01';
        msg += body;

        // Compute checksum.
        uint32_t sum = 0;
        for (char c : msg) sum += uint8_t(c);
        sum %= 256;

        char cs[8];
        std::snprintf(cs, sizeof(cs), "10=%03u\x01", sum);
        msg += cs;
        return msg;
    }
};

// ── Session ───────────────────────────────────────────────────────────────
class Session {
public:
    explicit Session(SessionConfig cfg, SessionCallbacks cbs)
        : cfg_{std::move(cfg)}, cbs_{std::move(cbs)} {}

    // ── Lifecycle ──────────────────────────────────────────────────────────

    void on_connect() {
        transition(SessionState::Connected);
        outgoing_seq_ = 1;
        if (cfg_.reset_on_logon) incoming_seq_ = 1;
    }

    void on_disconnect() {
        transition(SessionState::NotConnected);
    }

    // Initiate logon.
    void logon() {
        if (state_ != SessionState::Connected) return;
        transition(SessionState::LoggingOn);

        std::string body;
        body += "108="; body += std::to_string(cfg_.heartbeat_interval_s); body += '\x01';
        if (cfg_.reset_on_logon) { body += "141=Y\x01"; }

        send("A", body);
    }

    // Initiate logout.
    void logout(std::string_view text = "") {
        if (state_ != SessionState::LoggedOn) return;
        transition(SessionState::LoggingOut);
        std::string body;
        if (!text.empty()) { body += "58="; body += text; body += '\x01'; }
        send("5", body);
    }

    // ── Incoming message processing ────────────────────────────────────────
    //
    // Call this for every raw FIX message received from the wire.
    // Returns true if the message was processed successfully.
    //
    bool on_recv(std::span<char const> raw) {
        // Parse header.
        auto result = Parser::parse(raw, [](Field) {});
        if (result.error != ParseError::Ok) {
            if (cbs_.on_error) cbs_.on_error(to_string(result.error));
            return false;
        }
        if (result.msg_type_char == 0 || result.seq_num == 0) {
            if (cbs_.on_error) cbs_.on_error("missing MsgType or MsgSeqNum");
            return false;
        }

        char             mt[2]    = { char(result.msg_type_char), 0 };
        std::string_view msg_type = mt;
        uint64_t         seq      = result.seq_num;

        // ── Sequence number check (except Sequence Reset) ─────────────────
        if (msg_type != "4") {  // "4" = SequenceReset
            if (seq < incoming_seq_) {
                // Duplicate — send Reject.
                send_reject(seq, "MsgSeqNum too low");
                return false;
            }
            if (seq > incoming_seq_) {
                // Gap — send Resend Request.
                if (cbs_.on_gap) cbs_.on_gap(incoming_seq_, seq);
                send_resend_request(incoming_seq_, seq - 1);
                return false;
            }
            // Correct sequence — advance.
            ++incoming_seq_;
        }

        // ── Dispatch by message type ──────────────────────────────────────
        if      (msg_type == "A") return handle_logon(raw, seq);
        else if (msg_type == "5") return handle_logout(raw, seq);
        else if (msg_type == "0") return handle_heartbeat(raw, seq);
        else if (msg_type == "1") return handle_test_request(raw, seq);
        else if (msg_type == "2") return handle_resend_request(raw, seq);
        else if (msg_type == "4") return handle_sequence_reset(raw, seq);
        else if (msg_type == "3") return handle_reject(raw, seq);
        else {
            // Application message.
            if (state_ == SessionState::LoggedOn && cbs_.on_message) {
                cbs_.on_message(result, raw);
            }
            last_recv_time_ = now();
            return true;
        }
    }

    // ── Heartbeat check — call periodically ───────────────────────────────
    //
    // Returns true if a Test Request was sent (peer may be unresponsive).
    //
    bool check_heartbeat() {
        if (state_ != SessionState::LoggedOn) return false;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now() - last_recv_time_).count();

        if (elapsed > cfg_.heartbeat_interval_s * 2) {
            // Peer unresponsive — send Test Request.
            if (!test_request_pending_) {
                std::string body = "112=TEST\x01";
                send("1", body);
                test_request_pending_ = true;
                return true;
            }
        } else if (elapsed > cfg_.heartbeat_interval_s) {
            // Send Heartbeat.
            send("0", "");
        }
        return false;
    }

    // ── Accessors ──────────────────────────────────────────────────────────
    [[nodiscard]] SessionState state()        const noexcept { return state_; }
    [[nodiscard]] uint64_t     incoming_seq() const noexcept { return incoming_seq_; }
    [[nodiscard]] uint64_t     outgoing_seq() const noexcept { return outgoing_seq_; }
    [[nodiscard]] bool         logged_on()    const noexcept { return state_ == SessionState::LoggedOn; }

private:
    // ── Message handlers ──────────────────────────────────────────────────

    bool handle_logon(std::span<char const>, uint64_t) {
        if (state_ == SessionState::LoggingOn || state_ == SessionState::Connected) {
            const bool we_are_acceptor = (state_ == SessionState::Connected);
            transition(SessionState::LoggedOn);
            last_recv_time_ = now();
            // Send Logon response if we didn't initiate.
            if (we_are_acceptor) {
                std::string body;
                body += "108="; body += std::to_string(cfg_.heartbeat_interval_s); body += '\x01';
                send("A", body);
            }
        }
        return true;
    }

    bool handle_logout(std::span<char const>, uint64_t) {
        if (state_ == SessionState::LoggedOn) {
            // Peer initiated logout — respond and disconnect.
            send("5", "");
            transition(SessionState::NotConnected);
        } else if (state_ == SessionState::LoggingOut) {
            // Our logout was acknowledged.
            transition(SessionState::NotConnected);
        }
        return true;
    }

    bool handle_heartbeat(std::span<char const>, uint64_t) {
        last_recv_time_ = now();
        test_request_pending_ = false;
        return true;
    }

    bool handle_test_request(std::span<char const> raw, uint64_t) {
        // Reply with Heartbeat carrying the same TestReqID (tag 112).
        std::string_view test_req_id = "TEST";
        // Extract tag 112 from raw.
        (void)Parser::parse(raw, [&](Field f) {
            if (f.tag == 112) test_req_id = f.value;
        });
        std::string body = "112=";
        body += test_req_id;
        body += '\x01';
        send("0", body);
        last_recv_time_ = now();
        return true;
    }

    bool handle_resend_request(std::span<char const> raw, uint64_t) {
        // For simplicity: respond with GapFill (SequenceReset).
        // In production: replay stored messages.
        uint64_t begin_seq = 0, end_seq = 0;
        (void)Parser::parse(raw, [&](Field f) {
            if (f.tag == 7)  std::from_chars(f.value.data(), f.value.data() + f.value.size(), begin_seq);
            if (f.tag == 16) std::from_chars(f.value.data(), f.value.data() + f.value.size(), end_seq);
        });
        // Send GapFill to current outgoing seq.
        std::string body;
        body += "123=Y\x01";  // GapFillFlag
        body += "36="; body += std::to_string(outgoing_seq_); body += '\x01';
        send("4", body);
        return true;
    }

    bool handle_sequence_reset(std::span<char const> raw, uint64_t) {
        uint64_t new_seq = 0;
        (void)Parser::parse(raw, [&](Field f) {
            if (f.tag == 36) std::from_chars(f.value.data(), f.value.data() + f.value.size(), new_seq);
        });
        if (new_seq > 0) incoming_seq_ = new_seq;
        return true;
    }

    bool handle_reject(std::span<char const> raw, uint64_t seq) {
        std::string_view reason = "rejected";
        (void)Parser::parse(raw, [&](Field f) {
            if (f.tag == 58) reason = f.value;
        });
        if (cbs_.on_reject) cbs_.on_reject(seq, reason);
        return true;
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    void send(std::string_view msg_type, std::string_view body) {
        auto msg = MessageBuilder::build(
            cfg_.begin_string, msg_type,
            cfg_.sender_comp_id, cfg_.target_comp_id,
            outgoing_seq_++, body);
        if (cbs_.on_send) cbs_.on_send(msg);
    }

    void send_reject(uint64_t ref_seq, std::string_view reason) {
        std::string body;
        body += "45="; body += std::to_string(ref_seq); body += '\x01';
        body += "58="; body += reason; body += '\x01';
        send("3", body);
    }

    void send_resend_request(uint64_t begin_seq, uint64_t end_seq) {
        std::string body;
        body += "7="; body += std::to_string(begin_seq); body += '\x01';
        body += "16="; body += std::to_string(end_seq); body += '\x01';
        send("2", body);
    }

    void transition(SessionState new_state) {
        if (state_ == new_state) return;
        if (cbs_.on_state_change) cbs_.on_state_change(state_, new_state);
        state_ = new_state;
    }

    static std::chrono::steady_clock::time_point now() noexcept {
        return std::chrono::steady_clock::now();
    }

    // ── State ─────────────────────────────────────────────────────────────
    SessionConfig    cfg_;
    SessionCallbacks cbs_;
    SessionState     state_     = SessionState::NotConnected;
    uint64_t         incoming_seq_ = 1;
    uint64_t         outgoing_seq_ = 1;
    bool             test_request_pending_ = false;
    std::chrono::steady_clock::time_point last_recv_time_ = std::chrono::steady_clock::now();
};

}  // namespace fix
