#pragma once
// include/fix/groups.hpp — Dictionary-driven repeating-group parser.
//
// FIX repeating groups are the part of the protocol a flat tag=value scanner
// cannot handle: the same tag appears once per entry, entries are delimited
// by the reappearance of the group's first tag, and groups nest
// (NoLegs → NoNestedPartyIDs → NoNestedPartySubIDs).
//
// This layer sits on top of Parser::parse's flat visitor and assigns each
// field a *path*: the stack of (count_tag, entry_index) it is inside.
//
//   453=2|448=A|447=D|452=1|448=B|447=D|452=2|
//        └── entry 0 ─────┘└── entry 1 ─────┘
//
// yields fields tagged {453/0}, {453/0}, {453/0}, {453/1}, {453/1}, {453/1}.
//
// Membership is decided by the dictionary (GroupDef::members). The algorithm:
//   - a count tag opens a group (pushes onto the path stack, entry = -1)
//   - a delimiter tag starts a new entry when no entry is open, or when that
//     tag has already been seen in the current entry (tags are unique within
//     an entry, so a repeat can only mean "next entry")
//   - a tag that is not a member of the innermost open group closes it
//     (pop) and is re-tested against the parent, repeatedly
//   - an entry count that disagrees with the declared count is an error
//
// No heap. The path stack is a fixed array; kMaxDepth is 8 (FIX 4.4 needs 4).
// include/fix/dictionary_fix44.hpp is generated from the QuickFIX FIX44.xml
// spec by tools/gen_dictionary.py.

#include "fix/parser.hpp"
#include <algorithm>
#include <array>

namespace fix {

inline constexpr int kMaxTag = 2048;                       // FIX 4.4 max tag is 956
inline constexpr int kTagBitsetWords = kMaxTag / 64;

struct GroupDef {
    int        count_tag;
    const int* delimiters;   // sorted; a group's first tag differs by message
                             // (NoMDEntries: 269 in Snapshot, 279 in Incremental)
    int        n_delimiters;
    const int* members;      // sorted ascending; includes nested count tags
    int        n_members;

    [[nodiscard]] bool has_member(int tag) const noexcept {
        return std::binary_search(members, members + n_members, tag);
    }
    [[nodiscard]] bool is_delimiter(int tag) const noexcept {
        return std::binary_search(delimiters, delimiters + n_delimiters, tag);
    }
};

struct Dictionary {
    const GroupDef* groups;
    size_t          n_groups;

    [[nodiscard]] const GroupDef* find(int count_tag) const noexcept {
        // Sorted by count_tag (generator guarantees); binary search.
        auto it = std::lower_bound(groups, groups + n_groups, count_tag,
            [](const GroupDef& g, int t) { return g.count_tag < t; });
        return (it != groups + n_groups && it->count_tag == count_tag) ? it : nullptr;
    }
};

enum class GroupError : uint8_t {
    Ok,
    CountMismatch,    // declared N entries, saw M
    MalformedCount,   // count tag value not an integer
    TooDeep,          // nesting exceeds kMaxDepth
    OrphanTag,        // entry field seen before delimiter (entry never opened)
};

struct PathElem { int count_tag; int entry; };

struct GroupPath {
    static constexpr int kMaxDepth = 8;
    std::array<PathElem, kMaxDepth> elems{};
    int depth = 0;

    [[nodiscard]] bool empty() const noexcept { return depth == 0; }
    [[nodiscard]] const PathElem& back() const noexcept { return elems[depth - 1]; }
    [[nodiscard]] bool operator==(const GroupPath& o) const noexcept {
        if (depth != o.depth) return false;
        for (int i = 0; i < depth; ++i)
            if (elems[i].count_tag != o.elems[i].count_tag || elems[i].entry != o.elems[i].entry) return false;
        return true;
    }
};

struct GroupResult {
    ParseResult parse;
    GroupError  group_error = GroupError::Ok;
    int         offending_tag = 0;
};

class GroupParser {
public:
    // Visitor signature: void(Field f, const GroupPath& path)
    // path.depth == 0 for top-level fields.
    template<typename Visitor>
    [[nodiscard]] static GroupResult
    parse(std::span<const char> msg, const Dictionary& dict, Visitor&& visit) noexcept {
        GroupResult out;
        struct Frame {
            const GroupDef* def; int declared; int seen;
            std::array<uint64_t, kTagBitsetWords> in_entry{};   // tags seen in current entry
            void reset_entry() noexcept { in_entry.fill(0); }
            bool mark(int tag) noexcept {              // returns true if already present
                if (tag < 0 || tag >= kMaxTag) return false;
                uint64_t& w = in_entry[size_t(tag) >> 6]; uint64_t b = uint64_t(1) << (tag & 63);
                bool was = w & b; w |= b; return was;
            }
        };
        std::array<Frame, GroupPath::kMaxDepth> frames{};
        GroupPath path;

        auto close_top = [&]() {
            const Frame& f = frames[path.depth - 1];
            if (f.seen != f.declared && out.group_error == GroupError::Ok) {
                out.group_error = GroupError::CountMismatch;
                out.offending_tag = f.def->count_tag;
            }
            --path.depth;
        };

        out.parse = Parser::parse(msg, [&](Field f) {
            if (out.group_error != GroupError::Ok) return;

            // Unwind: close groups this tag is not a member of.
            while (path.depth > 0) {
                Frame& top = frames[path.depth - 1];
                if (top.def->has_member(f.tag)) {
                    bool no_entry = path.elems[path.depth - 1].entry < 0;
                    bool repeat   = !no_entry && top.mark(f.tag);
                    if (top.def->is_delimiter(f.tag) && (no_entry || repeat)) {
                        ++top.seen;
                        top.reset_entry(); top.mark(f.tag);
                        path.elems[path.depth - 1].entry = top.seen - 1;
                        break;
                    }
                    if (no_entry) { out.group_error = GroupError::OrphanTag; out.offending_tag = f.tag; return; }
                    if (repeat)   { close_top(); continue; }   // duplicate non-delimiter: entry is over
                    break;
                }
                close_top();
            }

            visit(f, path);

            // Open a nested/top-level group if this is a count tag.
            if (const GroupDef* g = dict.find(f.tag)) {
                auto n = Parser::parse_int(f.value);
                if (!n || *n < 0) { out.group_error = GroupError::MalformedCount; out.offending_tag = f.tag; return; }
                if (path.depth >= GroupPath::kMaxDepth) { out.group_error = GroupError::TooDeep; out.offending_tag = f.tag; return; }
                frames[path.depth] = Frame{g, int(*n), 0, {}};
                path.elems[path.depth] = PathElem{f.tag, -1};
                ++path.depth;
                if (*n == 0) close_top();   // NoXXX=0 is legal and opens nothing
            }
        });

        while (path.depth > 0) close_top();
        return out;
    }
};

// ── Materialised message (optional convenience, fixed capacity) ─────────────
//
// For callers that want random access rather than a visitor. Still no heap:
// kMaxFields entries of (tag, value, path) in a stack array.

template<int kMaxFields = 256>
struct Message {
    struct Entry { Field field; GroupPath path; };
    std::array<Entry, kMaxFields> entries{};
    int count = 0;
    GroupResult result;
    bool overflow = false;

    [[nodiscard]] static Message from(std::span<const char> msg, const Dictionary& dict) noexcept {
        Message m;
        m.result = GroupParser::parse(msg, dict, [&](Field f, const GroupPath& p) {
            if (m.count < kMaxFields) m.entries[m.count++] = Entry{f, p};
            else m.overflow = true;
        });
        return m;
    }

    // First value of `tag` at the top level (depth 0).
    [[nodiscard]] std::optional<std::string_view> get(int tag) const noexcept {
        for (int i = 0; i < count; ++i)
            if (entries[i].field.tag == tag && entries[i].path.empty()) return entries[i].field.value;
        return std::nullopt;
    }

    // Value of `tag` inside entry `idx` of group `count_tag` at top level.
    [[nodiscard]] std::optional<std::string_view>
    get(int count_tag, int idx, int tag) const noexcept {
        for (int i = 0; i < count; ++i) {
            const auto& e = entries[i];
            if (e.field.tag == tag && e.path.depth == 1 &&
                e.path.elems[0].count_tag == count_tag && e.path.elems[0].entry == idx)
                return e.field.value;
        }
        return std::nullopt;
    }

    // Number of entries observed for a top-level group.
    [[nodiscard]] int group_size(int count_tag) const noexcept {
        int mx = -1;
        for (int i = 0; i < count; ++i) {
            const auto& p = entries[i].path;
            if (p.depth >= 1 && p.elems[0].count_tag == count_tag) mx = std::max(mx, p.elems[0].entry);
        }
        return mx + 1;
    }
};

} // namespace fix
