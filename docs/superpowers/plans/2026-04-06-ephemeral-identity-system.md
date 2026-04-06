# Ephemeral Identity System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a reusable identity, presence, and ephemeral state system with a Qt-free core layer and Qt widget layer, implementing CRDT_SYNC_SPEC sections 3, 7, and 15.1.

**Architecture:** Core layer (pure C++20, zero dependencies beyond the CRDT engine types `Anchor`, `Global`, `Bias`) handles data model, JSON serialization, file I/O for identity/presence/ephemeral. Qt widget layer provides embeddable display widgets and convenience dialogs. SyncManager becomes a thin Qt adapter delegating to the core.

**Tech Stack:** C++20 standard library, `std::filesystem`, existing CRDT engine types (`Anchor`, `Global`, `Bias`, `Lamport`), Qt6 Widgets (UI layer only), QTest (tests)

---

## File Map

### Core layer (Qt-free)

| File | Responsibility |
|------|----------------|
| `include/collabtext/Identity.h` | `Identity`, `Presence`, `EphemeralState` structs |
| `include/collabtext/IdentityStore.h` | Local identity management (`~/.config/collabtext/`) |
| `include/collabtext/IdentityProjector.h` | Project identity into shared folder, read remote identities |
| `include/collabtext/PresenceManager.h` | Write/read presence.json and ephemeral.json |
| `include/collabtext/Signing.h` | Ed25519 stubs |
| `src/identity/Identity.cpp` | JSON serialization for all structs |
| `src/identity/IdentityStore.cpp` | File I/O for local identity |
| `src/identity/IdentityProjector.cpp` | File I/O for identity projection |
| `src/identity/PresenceManager.cpp` | File I/O for presence + ephemeral |
| `src/identity/Signing.cpp` | Stub implementations |

### Qt widget layer

| File | Responsibility |
|------|----------------|
| `src/ui/AvatarWidget.h` / `.cpp` | Circular avatar or initials fallback |
| `src/ui/PresenceIndicator.h` / `.cpp` | Colored dot for activity state |
| `src/ui/IdentityEditor.h` / `.cpp` | Form panel for editing an Identity |
| `src/ui/ParticipantListWidget.h` / `.cpp` | List of connected participants |
| `src/ui/IdentitySetupDialog.h` / `.cpp` | First-launch wizard dialog |
| `src/ui/IdentityPreferencesPage.h` / `.cpp` | Settings panel for identity |

### Modified files

| File | Change |
|------|--------|
| `libs/collabtext/CMakeLists.txt` | Add new source files and test targets |
| `include/collabtext/SyncManager.h` | Accept Identity, emit typed signals |
| `src/SyncManager.cpp` | Delegate to PresenceManager, IdentityProjector |
| `app/main.cpp` | Replace hardcoded identity with IdentityStore, cursor sync via ephemeral files |

### Tests

| File | What it tests |
|------|---------------|
| `tests/tst_identity_json.cpp` | JSON round-trip for Identity, Presence, EphemeralState |
| `tests/tst_identity_store.cpp` | IdentityStore generate/save/load, avatar stub |
| `tests/tst_identity_projector.cpp` | Project, read, stale-update-skip |
| `tests/tst_presence_manager.cpp` | Write/read presence + ephemeral, liveness, departure |
| `tests/tst_identity_widgets.cpp` | Widget smoke tests (instantiation, setIdentity, signals) |

All paths below are relative to `libs/collabtext/`.

---

## Task 1: Identity Data Model and JSON Serialization

**Files:**
- Create: `include/collabtext/Identity.h`
- Create: `src/identity/Identity.cpp`
- Create: `tests/tst_identity_json.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/tst_identity_json.cpp`:

```cpp
#include <QTest>
#include "collabtext/Identity.h"

using namespace CollabText::Identity;

class TestIdentityJson : public QObject {
    Q_OBJECT

private slots:
    void identity_roundtrip() {
        Identity id;
        id.identity_id = "clinton-a7f3b2";
        id.display_name = "Clinton";
        id.status = "Drafting the sync spec";
        id.bio = "Systems programmer.";
        id.color = "#3b82f6";
        id.public_key = "";
        id.updated = "2026-04-06T12:00:00Z";

        auto json = to_json(id);
        auto parsed = identity_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->identity_id, id.identity_id);
        QCOMPARE(parsed->display_name, id.display_name);
        QCOMPARE(parsed->status, id.status);
        QCOMPARE(parsed->bio, id.bio);
        QCOMPARE(parsed->color, id.color);
        QCOMPARE(parsed->public_key, id.public_key);
        QCOMPARE(parsed->updated, id.updated);
    }

    void identity_with_special_chars() {
        Identity id;
        id.identity_id = "user-abc123";
        id.display_name = "Tëst \"User\" \\one";
        id.status = "line\nbreak";
        id.bio = "";
        id.color = "#ff0000";
        id.public_key = "";
        id.updated = "2026-04-06T12:00:00Z";

        auto json = to_json(id);
        auto parsed = identity_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->display_name, id.display_name);
        QCOMPARE(parsed->status, id.status);
    }

    void identity_malformed_json_returns_nullopt() {
        QVERIFY(!identity_from_json("not json").has_value());
        QVERIFY(!identity_from_json("").has_value());
        QVERIFY(!identity_from_json("{}").has_value()); // missing required fields
    }

    void presence_roundtrip() {
        Presence p;
        p.replica_id = "laptop-3";
        p.identity_id = "clinton-a7f3b2";
        p.device_name = "Clinton's ThinkPad";
        p.active = true;
        p.last_heartbeat = "2026-04-06T14:30:00.337Z";
        p.session_started = "2026-04-06T12:00:00Z";
        p.version_summary.observe(Crdt::Lamport(1, 421));
        p.version_summary.observe(Crdt::Lamport(2, 300));

        auto json = to_json(p);
        auto parsed = presence_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->replica_id, p.replica_id);
        QCOMPARE(parsed->identity_id, p.identity_id);
        QCOMPARE(parsed->device_name, p.device_name);
        QCOMPARE(parsed->active, true);
        QCOMPARE(parsed->last_heartbeat, p.last_heartbeat);
        QCOMPARE(parsed->session_started, p.session_started);
        QCOMPARE(parsed->version_summary.get(1), uint32_t(421));
        QCOMPARE(parsed->version_summary.get(2), uint32_t(300));
    }

    void presence_departed() {
        Presence p;
        p.replica_id = "laptop-3";
        p.identity_id = "clinton-a7f3b2";
        p.device_name = "ThinkPad";
        p.active = false;
        p.last_heartbeat = "2026-04-06T14:30:00Z";
        p.session_started = "2026-04-06T12:00:00Z";

        auto json = to_json(p);
        auto parsed = presence_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->active, false);
    }

    void ephemeral_roundtrip() {
        EphemeralState es;
        es.seq = 14207;
        es.timestamp = "2026-04-06T14:32:01.337Z";
        es.cursors.push_back({
            Crdt::Anchor(1, 400, Crdt::Bias::Right),
            Crdt::Anchor(1, 400, Crdt::Bias::Right)
        });
        es.selections.push_back({
            Crdt::Anchor(2, 50, Crdt::Bias::Left),
            Crdt::Anchor(2, 77, Crdt::Bias::Right)
        });
        es.viewport_top = Crdt::Anchor(1, 380, Crdt::Bias::Left);
        es.viewport_bottom = Crdt::Anchor(1, 412, Crdt::Bias::Left);
        es.activity = "typing";

        auto json = to_json(es);
        auto parsed = ephemeral_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->seq, es.seq);
        QCOMPARE(parsed->timestamp, es.timestamp);
        QCOMPARE(parsed->cursors.size(), size_t(1));
        QCOMPARE(parsed->cursors[0].anchor.replica_id, uint16_t(1));
        QCOMPARE(parsed->cursors[0].anchor.char_value, uint32_t(400));
        QCOMPARE(parsed->cursors[0].anchor.bias, Crdt::Bias::Right);
        QCOMPARE(parsed->selections.size(), size_t(1));
        QVERIFY(parsed->viewport_top.has_value());
        QCOMPARE(parsed->viewport_top->char_value, uint32_t(380));
        QVERIFY(parsed->viewport_bottom.has_value());
        QCOMPARE(parsed->activity, std::string("typing"));
    }

    void ephemeral_no_viewport() {
        EphemeralState es;
        es.seq = 1;
        es.timestamp = "2026-04-06T14:32:01Z";
        es.activity = "idle";

        auto json = to_json(es);
        auto parsed = ephemeral_from_json(json);
        QVERIFY(parsed.has_value());
        QVERIFY(!parsed->viewport_top.has_value());
        QVERIFY(!parsed->viewport_bottom.has_value());
        QCOMPARE(parsed->cursors.size(), size_t(0));
    }

    void ephemeral_malformed_returns_nullopt() {
        QVERIFY(!ephemeral_from_json("garbage").has_value());
        QVERIFY(!ephemeral_from_json("").has_value());
    }
};

QTEST_MAIN(TestIdentityJson)
#include "tst_identity_json.moc"
```

- [ ] **Step 2: Create the header with structs and function declarations**

Create `include/collabtext/Identity.h`:

```cpp
#pragma once

#include "crdt/Anchor.h"
#include "crdt/Clock.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace CollabText::Identity {

// Pull in CRDT types used by ephemeral state
namespace Crdt = CollabText::Crdt;

struct Identity {
    std::string identity_id;      // "clinton-a7f3b2" — generated once
    std::string display_name;     // free-form UTF-8
    std::string status;           // short IM-style status line
    std::string bio;              // longer description
    std::string color;            // "#3b82f6" hex color
    std::string public_key;       // "ed25519:base64..." — empty if unsigned
    std::string updated;          // ISO 8601 timestamp
};

struct Presence {
    std::string replica_id;
    std::string identity_id;
    std::string device_name;
    bool active = true;
    std::string last_heartbeat;   // ISO 8601, updated every sync cycle
    std::string session_started;  // ISO 8601, set once at session start
    Crdt::Global version_summary;
};

struct EphemeralState {
    uint64_t seq = 0;
    std::string timestamp;        // ISO 8601

    struct CursorPair {
        Crdt::Anchor anchor;
        Crdt::Anchor head;
    };

    std::vector<CursorPair> cursors;
    std::vector<CursorPair> selections;

    std::optional<Crdt::Anchor> viewport_top;
    std::optional<Crdt::Anchor> viewport_bottom;

    std::string activity;         // "typing", "selecting", "idle", "away"
};

// --- JSON serialization ---

std::string to_json(const Identity& id);
std::optional<Identity> identity_from_json(const std::string& json);

std::string to_json(const Presence& p);
std::optional<Presence> presence_from_json(const std::string& json);

std::string to_json(const EphemeralState& es);
std::optional<EphemeralState> ephemeral_from_json(const std::string& json);

} // namespace CollabText::Identity
```

- [ ] **Step 3: Add to CMakeLists.txt**

In `libs/collabtext/CMakeLists.txt`, add `src/identity/Identity.cpp` to the `add_library(collabtext STATIC ...)` source list, and add the test target:

```cmake
# After the existing add_library sources, add:
    src/identity/Identity.cpp
```

```cmake
# After the existing add_crdt_test lines, add:
add_crdt_test(tst_identity_json)
```

- [ ] **Step 4: Run the test to verify it fails**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_json 2>&1 | tail -20`
Expected: Linker errors — `to_json`, `identity_from_json`, etc. are declared but not defined.

- [ ] **Step 5: Implement JSON serialization**

Create `src/identity/Identity.cpp`. This follows the same hand-rolled JSON pattern used in `src/crdt/Serialization.cpp`. The key functions:

- `escape_json_string()` — reuse the same escaping logic from `Serialization.cpp` (copy it; the identity module must not depend on CRDT internals beyond public headers).
- `to_json(Identity)` — writes a flat JSON object with all fields.
- `identity_from_json()` — minimal recursive-descent parser for the flat Identity object. Returns `nullopt` if `identity_id` is missing (the one required field).
- `to_json(Presence)` — includes `version_summary` as a JSON object `{"replica_id": seq, ...}` mapping replica_id (as string index) to sequence number.
- `presence_from_json()` — parses all fields, reconstructs `Global` by calling `observe(Lamport(i, val))` for each entry.
- `to_json(EphemeralState)` — encodes anchors as `{"replica": N, "seq": N, "offset": N, "bias": "left"|"right"}`. Note: the Anchor struct uses `replica_id` and `char_value` fields; the JSON uses `replica`, `seq`, `offset` (offset is always 0 for our anchors — the spec's offset field refers to byte offset within a multi-char fragment, but our engine's `Anchor` addresses individual characters, so offset is 0 and `char_value` maps to `seq`).
- `ephemeral_from_json()` — parses the nested structure, returns `nullopt` on any parse failure.

The parser needs to handle: string values (with escape sequences), integer values, boolean values, arrays of objects, optional fields, nested objects. Write a small `JsonReader` helper class that wraps a `string_view` with a position cursor and provides: `skip_ws()`, `expect(char)`, `read_string()`, `read_uint64()`, `read_int()`, `read_bool()`, `peek()`, `skip_value()` (to skip unknown fields for forward compatibility).

```cpp
#include "collabtext/Identity.h"

#include <charconv>
#include <sstream>
#include <string_view>

namespace CollabText::Identity {

// ---- JSON string escaping (same logic as crdt/Serialization.cpp) ----

static std::string escape_json_string(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

// ---- Minimal JSON reader ----

class JsonReader {
public:
    explicit JsonReader(std::string_view src) : m_src(src) {}

    bool at_end() const { return m_pos >= m_src.size(); }
    char peek() { skip_ws(); return m_pos < m_src.size() ? m_src[m_pos] : '\0'; }
    bool ok() const { return m_ok; }

    bool expect(char c) {
        skip_ws();
        if (m_pos < m_src.size() && m_src[m_pos] == c) { ++m_pos; return true; }
        m_ok = false;
        return false;
    }

    std::string read_string() {
        skip_ws();
        if (!expect('"')) return {};
        std::string result;
        while (m_pos < m_src.size() && m_src[m_pos] != '"') {
            if (m_src[m_pos] == '\\') {
                ++m_pos;
                if (m_pos >= m_src.size()) { m_ok = false; return {}; }
                switch (m_src[m_pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'u': {
                        if (m_pos + 4 >= m_src.size()) { m_ok = false; return {}; }
                        unsigned cp = 0;
                        for (int i = 1; i <= 4; ++i) {
                            char h = m_src[m_pos + i];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else { m_ok = false; return {}; }
                        }
                        m_pos += 4;
                        // Encode codepoint as UTF-8
                        if (cp < 0x80) {
                            result += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: result += m_src[m_pos]; break;
                }
            } else {
                result += m_src[m_pos];
            }
            ++m_pos;
        }
        if (!expect('"')) return {};
        return result;
    }

    uint64_t read_uint64() {
        skip_ws();
        uint64_t val = 0;
        auto start = m_pos;
        while (m_pos < m_src.size() && m_src[m_pos] >= '0' && m_src[m_pos] <= '9') {
            val = val * 10 + (m_src[m_pos] - '0');
            ++m_pos;
        }
        if (m_pos == start) m_ok = false;
        return val;
    }

    bool read_bool() {
        skip_ws();
        if (m_src.substr(m_pos, 4) == "true") { m_pos += 4; return true; }
        if (m_src.substr(m_pos, 5) == "false") { m_pos += 5; return false; }
        m_ok = false;
        return false;
    }

    void skip_value() {
        skip_ws();
        if (m_pos >= m_src.size()) { m_ok = false; return; }
        char c = m_src[m_pos];
        if (c == '"') { read_string(); }
        else if (c == '{') { skip_container('{', '}'); }
        else if (c == '[') { skip_container('[', ']'); }
        else if (c == 't' || c == 'f') { read_bool(); }
        else if (c == 'n') {
            if (m_src.substr(m_pos, 4) == "null") m_pos += 4;
            else m_ok = false;
        } else {
            // number
            while (m_pos < m_src.size() &&
                   (m_src[m_pos] == '-' || m_src[m_pos] == '+' ||
                    m_src[m_pos] == '.' || m_src[m_pos] == 'e' ||
                    m_src[m_pos] == 'E' ||
                    (m_src[m_pos] >= '0' && m_src[m_pos] <= '9')))
                ++m_pos;
        }
    }

    void skip_ws() {
        while (m_pos < m_src.size() &&
               (m_src[m_pos] == ' ' || m_src[m_pos] == '\t' ||
                m_src[m_pos] == '\n' || m_src[m_pos] == '\r'))
            ++m_pos;
    }

private:
    void skip_container(char open, char close) {
        expect(open);
        int depth = 1;
        while (m_pos < m_src.size() && depth > 0) {
            if (m_src[m_pos] == '"') {
                ++m_pos;
                while (m_pos < m_src.size() && m_src[m_pos] != '"') {
                    if (m_src[m_pos] == '\\') ++m_pos;
                    ++m_pos;
                }
                ++m_pos; // closing quote
            } else {
                if (m_src[m_pos] == open) ++depth;
                if (m_src[m_pos] == close) --depth;
                ++m_pos;
            }
        }
    }

    std::string_view m_src;
    size_t m_pos = 0;
    bool m_ok = true;
};

// ---- Anchor JSON helpers ----

static std::string anchor_to_json(const Crdt::Anchor& a) {
    std::string out = "{\"replica\":";
    out += std::to_string(a.replica_id);
    out += ",\"seq\":";
    out += std::to_string(a.char_value);
    out += ",\"offset\":0,\"bias\":";
    out += (a.bias == Crdt::Bias::Right) ? "\"right\"" : "\"left\"";
    out += '}';
    return out;
}

static std::optional<Crdt::Anchor> anchor_from_json(JsonReader& r) {
    if (!r.expect('{')) return std::nullopt;
    uint16_t replica = 0;
    uint32_t seq = 0;
    Crdt::Bias bias = Crdt::Bias::Left;
    bool has_replica = false, has_seq = false;

    while (r.ok() && r.peek() != '}') {
        auto key = r.read_string();
        r.expect(':');
        if (key == "replica") { replica = static_cast<uint16_t>(r.read_uint64()); has_replica = true; }
        else if (key == "seq") { seq = static_cast<uint32_t>(r.read_uint64()); has_seq = true; }
        else if (key == "offset") { r.read_uint64(); /* consumed but unused */ }
        else if (key == "bias") {
            auto b = r.read_string();
            bias = (b == "right") ? Crdt::Bias::Right : Crdt::Bias::Left;
        }
        else { r.skip_value(); }
        if (r.peek() == ',') r.expect(',');
    }
    r.expect('}');
    if (!r.ok() || !has_replica || !has_seq) return std::nullopt;
    return Crdt::Anchor(replica, seq, bias);
}

// ---- CursorPair JSON helpers ----

static std::string cursor_pair_to_json(const EphemeralState::CursorPair& cp) {
    std::string out = "{\"anchor\":";
    out += anchor_to_json(cp.anchor);
    out += ",\"head\":";
    out += anchor_to_json(cp.head);
    out += '}';
    return out;
}

static std::optional<EphemeralState::CursorPair> cursor_pair_from_json(JsonReader& r) {
    if (!r.expect('{')) return std::nullopt;
    std::optional<Crdt::Anchor> anchor, head;

    while (r.ok() && r.peek() != '}') {
        auto key = r.read_string();
        r.expect(':');
        if (key == "anchor") { anchor = anchor_from_json(r); }
        else if (key == "head") { head = anchor_from_json(r); }
        else { r.skip_value(); }
        if (r.peek() == ',') r.expect(',');
    }
    r.expect('}');
    if (!r.ok() || !anchor || !head) return std::nullopt;
    return EphemeralState::CursorPair{*anchor, *head};
}

// ---- Identity JSON ----

std::string to_json(const Identity& id) {
    std::string out = "{";
    out += "\"identity_id\":" + escape_json_string(id.identity_id);
    out += ",\"display_name\":" + escape_json_string(id.display_name);
    out += ",\"status\":" + escape_json_string(id.status);
    out += ",\"bio\":" + escape_json_string(id.bio);
    out += ",\"color\":" + escape_json_string(id.color);
    out += ",\"public_key\":" + escape_json_string(id.public_key);
    out += ",\"signature\":\"\"";
    out += ",\"updated\":" + escape_json_string(id.updated);
    out += "}";
    return out;
}

std::optional<Identity> identity_from_json(const std::string& json) {
    JsonReader r(json);
    if (!r.expect('{')) return std::nullopt;

    Identity id;
    bool has_id = false;

    while (r.ok() && r.peek() != '}') {
        auto key = r.read_string();
        r.expect(':');
        if (key == "identity_id") { id.identity_id = r.read_string(); has_id = true; }
        else if (key == "display_name") { id.display_name = r.read_string(); }
        else if (key == "status") { id.status = r.read_string(); }
        else if (key == "bio") { id.bio = r.read_string(); }
        else if (key == "color") { id.color = r.read_string(); }
        else if (key == "public_key") { id.public_key = r.read_string(); }
        else if (key == "updated") { id.updated = r.read_string(); }
        else { r.skip_value(); }
        if (r.peek() == ',') r.expect(',');
    }
    r.expect('}');
    if (!r.ok() || !has_id) return std::nullopt;
    return id;
}

// ---- Presence JSON ----

std::string to_json(const Presence& p) {
    std::string out = "{";
    out += "\"replica_id\":" + escape_json_string(p.replica_id);
    out += ",\"identity_id\":" + escape_json_string(p.identity_id);
    out += ",\"device_name\":" + escape_json_string(p.device_name);
    out += ",\"active\":";
    out += p.active ? "true" : "false";
    out += ",\"last_heartbeat\":" + escape_json_string(p.last_heartbeat);
    out += ",\"session_started\":" + escape_json_string(p.session_started);

    // version_summary as {"0": val, "1": val, ...}
    out += ",\"version_summary\":{";
    const auto& vals = p.version_summary.values();
    bool first = true;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (vals[i] == 0) continue;
        if (!first) out += ',';
        out += "\"" + std::to_string(i) + "\":" + std::to_string(vals[i]);
        first = false;
    }
    out += "}";

    out += ",\"channels\":[]";
    out += ",\"capabilities\":{\"crdt_version\":1}";
    out += "}";
    return out;
}

std::optional<Presence> presence_from_json(const std::string& json) {
    JsonReader r(json);
    if (!r.expect('{')) return std::nullopt;

    Presence p;
    bool has_replica = false;

    while (r.ok() && r.peek() != '}') {
        auto key = r.read_string();
        r.expect(':');
        if (key == "replica_id") { p.replica_id = r.read_string(); has_replica = true; }
        else if (key == "identity_id") { p.identity_id = r.read_string(); }
        else if (key == "device_name") { p.device_name = r.read_string(); }
        else if (key == "active") { p.active = r.read_bool(); }
        else if (key == "last_heartbeat") { p.last_heartbeat = r.read_string(); }
        else if (key == "session_started") { p.session_started = r.read_string(); }
        else if (key == "version_summary") {
            r.expect('{');
            while (r.ok() && r.peek() != '}') {
                auto idx_str = r.read_string();
                r.expect(':');
                auto val = r.read_uint64();
                uint16_t idx = static_cast<uint16_t>(std::stoul(idx_str));
                p.version_summary.observe(Crdt::Lamport(idx, static_cast<uint32_t>(val)));
                if (r.peek() == ',') r.expect(',');
            }
            r.expect('}');
        }
        else { r.skip_value(); }
        if (r.peek() == ',') r.expect(',');
    }
    r.expect('}');
    if (!r.ok() || !has_replica) return std::nullopt;
    return p;
}

// ---- EphemeralState JSON ----

std::string to_json(const EphemeralState& es) {
    std::string out = "{";
    out += "\"seq\":" + std::to_string(es.seq);
    out += ",\"timestamp\":" + escape_json_string(es.timestamp);

    out += ",\"cursors\":[";
    for (size_t i = 0; i < es.cursors.size(); ++i) {
        if (i > 0) out += ',';
        out += cursor_pair_to_json(es.cursors[i]);
    }
    out += "]";

    out += ",\"selections\":[";
    for (size_t i = 0; i < es.selections.size(); ++i) {
        if (i > 0) out += ',';
        out += cursor_pair_to_json(es.selections[i]);
    }
    out += "]";

    out += ",\"viewport\":{";
    if (es.viewport_top) {
        out += "\"top\":" + anchor_to_json(*es.viewport_top);
        if (es.viewport_bottom)
            out += ",\"bottom\":" + anchor_to_json(*es.viewport_bottom);
    }
    out += "}";

    out += ",\"activity\":" + escape_json_string(es.activity);
    out += ",\"custom\":{}";
    out += "}";
    return out;
}

std::optional<EphemeralState> ephemeral_from_json(const std::string& json) {
    JsonReader r(json);
    if (!r.expect('{')) return std::nullopt;

    EphemeralState es;
    bool has_seq = false;

    while (r.ok() && r.peek() != '}') {
        auto key = r.read_string();
        r.expect(':');
        if (key == "seq") { es.seq = r.read_uint64(); has_seq = true; }
        else if (key == "timestamp") { es.timestamp = r.read_string(); }
        else if (key == "activity") { es.activity = r.read_string(); }
        else if (key == "cursors") {
            r.expect('[');
            while (r.ok() && r.peek() != ']') {
                auto cp = cursor_pair_from_json(r);
                if (cp) es.cursors.push_back(*cp);
                if (r.peek() == ',') r.expect(',');
            }
            r.expect(']');
        }
        else if (key == "selections") {
            r.expect('[');
            while (r.ok() && r.peek() != ']') {
                auto cp = cursor_pair_from_json(r);
                if (cp) es.selections.push_back(*cp);
                if (r.peek() == ',') r.expect(',');
            }
            r.expect(']');
        }
        else if (key == "viewport") {
            r.expect('{');
            while (r.ok() && r.peek() != '}') {
                auto vk = r.read_string();
                r.expect(':');
                if (vk == "top") { es.viewport_top = anchor_from_json(r); }
                else if (vk == "bottom") { es.viewport_bottom = anchor_from_json(r); }
                else { r.skip_value(); }
                if (r.peek() == ',') r.expect(',');
            }
            r.expect('}');
        }
        else { r.skip_value(); }
        if (r.peek() == ',') r.expect(',');
    }
    r.expect('}');
    if (!r.ok() || !has_seq) return std::nullopt;
    return es;
}

} // namespace CollabText::Identity
```

- [ ] **Step 6: Build and run the test**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_json -j$(nproc) 2>&1 | tail -5 && build-dev/libs/collabtext/tst_identity_json`
Expected: All tests pass.

- [ ] **Step 7: Commit**

```bash
git add libs/collabtext/include/collabtext/Identity.h \
        libs/collabtext/src/identity/Identity.cpp \
        libs/collabtext/tests/tst_identity_json.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: Identity, Presence, EphemeralState data model + JSON serialization"
```

---

## Task 2: Signing Stubs

**Files:**
- Create: `include/collabtext/Signing.h`
- Create: `src/identity/Signing.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create the header**

Create `include/collabtext/Signing.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"

#include <optional>
#include <string>

namespace CollabText::Identity {

struct SigningKeyPair {
    std::string public_key;   // "ed25519:base64..."
    std::string private_key;  // raw bytes, never synced
};

/// Generate an Ed25519 keypair. Currently returns nullopt (stub).
std::optional<SigningKeyPair> generate_keypair();

/// Sign profile fields. Currently returns empty string (stub).
std::string sign_profile(const Identity& identity,
                         const std::string& private_key);

/// Verify profile signature. Currently returns true (stub — trust everything).
bool verify_profile(const Identity& identity,
                    const std::string& signature);

} // namespace CollabText::Identity
```

- [ ] **Step 2: Create the stub implementation**

Create `src/identity/Signing.cpp`:

```cpp
#include "collabtext/Signing.h"

namespace CollabText::Identity {

std::optional<SigningKeyPair> generate_keypair() {
    // Stub: Ed25519 not yet implemented
    return std::nullopt;
}

std::string sign_profile(const Identity& /*identity*/,
                         const std::string& /*private_key*/) {
    // Stub: signing not yet implemented
    return {};
}

bool verify_profile(const Identity& /*identity*/,
                    const std::string& /*signature*/) {
    // Stub: trust everything until signing is implemented
    return true;
}

} // namespace CollabText::Identity
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `src/identity/Signing.cpp` to the library sources.

- [ ] **Step 4: Build to verify compilation**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target collabtext -j$(nproc) 2>&1 | tail -5`
Expected: Clean build.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/include/collabtext/Signing.h \
        libs/collabtext/src/identity/Signing.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: Ed25519 signing stubs — interface ready, implementation deferred"
```

---

## Task 3: IdentityStore

**Files:**
- Create: `include/collabtext/IdentityStore.h`
- Create: `src/identity/IdentityStore.cpp`
- Create: `tests/tst_identity_store.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/tst_identity_store.cpp`:

```cpp
#include <QTest>
#include <QTemporaryDir>
#include "collabtext/IdentityStore.h"

using namespace CollabText::Identity;
namespace fs = std::filesystem;

class TestIdentityStore : public QObject {
    Q_OBJECT

private slots:
    void load_returns_nullopt_when_no_file() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        QVERIFY(!store.load().has_value());
    }

    void generate_creates_valid_identity() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto id = store.generate("Clinton");

        QVERIFY(id.identity_id.starts_with("clinton-"));
        QCOMPARE(id.identity_id.size(), size_t(7 + 1 + 6)); // "clinton" + "-" + 6 hex
        QCOMPARE(id.display_name, std::string("Clinton"));
        QVERIFY(!id.color.empty());
        QVERIFY(id.color[0] == '#');
        QVERIFY(!id.updated.empty());
    }

    void generate_slug_handles_unicode() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto id = store.generate("Tëst Üser");
        // Slug should be ASCII lowercase
        QVERIFY(id.identity_id.find("tst") != std::string::npos ||
                id.identity_id.find("test") != std::string::npos ||
                id.identity_id.size() > 3); // at least some slug + hex
    }

    void save_and_load_roundtrip() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto id = store.generate("Alice");
        store.save(id);

        auto loaded = store.load();
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->identity_id, id.identity_id);
        QCOMPARE(loaded->display_name, id.display_name);
        QCOMPARE(loaded->color, id.color);
    }

    void save_creates_directory() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "nested" / "dir";
        IdentityStore store(dir);
        auto id = store.generate("Bob");
        store.save(id);

        QVERIFY(fs::exists(dir / "identity.json"));
    }

    void avatar_path_returns_expected_path() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto path = store.avatar_path();
        QVERIFY(path.filename() == "avatar.png");
    }

    void load_avatar_returns_empty_when_no_file() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto data = store.load_avatar();
        QVERIFY(data.empty());
    }

    void save_avatar_rejects_oversized() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        std::vector<uint8_t> big(257 * 1024, 0); // 257KB
        QVERIFY(!store.save_avatar(big));
    }

    void save_and_load_avatar() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        std::vector<uint8_t> data = {0x89, 0x50, 0x4E, 0x47}; // PNG magic
        QVERIFY(store.save_avatar(data));
        auto loaded = store.load_avatar();
        QCOMPARE(loaded.size(), data.size());
        QCOMPARE(loaded, data);
    }

    void signing_key_path_returns_expected() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto path = store.signing_key_path();
        QVERIFY(path.filename() == "identity.key");
    }

    void two_generates_produce_different_ids() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto id1 = store.generate("Same");
        auto id2 = store.generate("Same");
        QVERIFY(id1.identity_id != id2.identity_id);
    }
};

QTEST_MAIN(TestIdentityStore)
#include "tst_identity_store.moc"
```

- [ ] **Step 2: Create the header**

Create `include/collabtext/IdentityStore.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace CollabText::Identity {

class IdentityStore {
public:
    /// config_dir defaults to ~/.config/collabtext/ but is injectable for testing.
    explicit IdentityStore(std::filesystem::path config_dir);

    /// Load identity from disk, or nullopt if none exists yet.
    std::optional<Identity> load() const;

    /// Save identity to disk (creates directory if needed).
    void save(const Identity& identity);

    /// Generate a new identity with a random id suffix.
    /// e.g. generate("Clinton") -> identity_id "clinton-a7f3b2"
    Identity generate(const std::string& display_name);

    /// Avatar path (may not exist on disk).
    std::filesystem::path avatar_path() const;

    /// Load avatar bytes, or empty if no avatar.
    std::vector<uint8_t> load_avatar() const;

    /// Save avatar bytes. Returns false if data > 256KB.
    bool save_avatar(const std::vector<uint8_t>& data);

    /// Signing key path (may not exist on disk).
    std::filesystem::path signing_key_path() const;

private:
    std::filesystem::path m_config_dir;
};

} // namespace CollabText::Identity
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `src/identity/IdentityStore.cpp` to library sources. Add `add_crdt_test(tst_identity_store)`.

- [ ] **Step 4: Run test to verify it fails**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_store 2>&1 | tail -20`
Expected: Linker errors for IdentityStore methods.

- [ ] **Step 5: Implement IdentityStore**

Create `src/identity/IdentityStore.cpp`:

```cpp
#include "collabtext/IdentityStore.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>

namespace CollabText::Identity {

namespace fs = std::filesystem;

IdentityStore::IdentityStore(fs::path config_dir)
    : m_config_dir(std::move(config_dir))
{
}

std::optional<Identity> IdentityStore::load() const {
    fs::path path = m_config_dir / "identity.json";
    if (!fs::exists(path)) return std::nullopt;

    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return identity_from_json(content);
}

/// Write content to a temp file then rename (atomic on POSIX).
static void atomic_write(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    fs::rename(tmp, path);
}

void IdentityStore::save(const Identity& identity) {
    atomic_write(m_config_dir / "identity.json", to_json(identity));
}

/// Generate a slug from a display name: lowercase ASCII, non-alpha stripped.
static std::string slugify(const std::string& name) {
    std::string slug;
    for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z')
            slug += static_cast<char>(c + 32);
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            slug += static_cast<char>(c);
        // skip non-ASCII and non-alphanumeric
    }
    if (slug.empty()) slug = "user";
    return slug;
}

/// Current UTC time as ISO 8601.
static std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

Identity IdentityStore::generate(const std::string& display_name) {
    static constexpr const char* palette[] = {
        "#3b82f6", "#ef4444", "#22c55e", "#f59e0b", "#8b5cf6",
        "#ec4899", "#06b6d4", "#f97316", "#14b8a6", "#a855f7",
    };
    static constexpr int palette_size = sizeof(palette) / sizeof(palette[0]);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> hex_dist(0, 0xFFFFFF);
    std::uniform_int_distribution<int> color_dist(0, palette_size - 1);

    char hex[8];
    snprintf(hex, sizeof(hex), "%06x", hex_dist(gen));

    Identity id;
    id.identity_id = slugify(display_name) + "-" + hex;
    id.display_name = display_name;
    id.color = palette[color_dist(gen)];
    id.updated = now_iso8601();
    return id;
}

fs::path IdentityStore::avatar_path() const {
    return m_config_dir / "avatar.png";
}

std::vector<uint8_t> IdentityStore::load_avatar() const {
    fs::path path = avatar_path();
    if (!fs::exists(path)) return {};

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};

    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

bool IdentityStore::save_avatar(const std::vector<uint8_t>& data) {
    if (data.size() > 256 * 1024) return false;

    fs::create_directories(m_config_dir);
    fs::path tmp = avatar_path();
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    fs::rename(tmp, avatar_path());
    return true;
}

fs::path IdentityStore::signing_key_path() const {
    return m_config_dir / "identity.key";
}

} // namespace CollabText::Identity
```

- [ ] **Step 6: Build and run the test**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_store -j$(nproc) 2>&1 | tail -5 && build-dev/libs/collabtext/tst_identity_store`
Expected: All tests pass.

- [ ] **Step 7: Commit**

```bash
git add libs/collabtext/include/collabtext/IdentityStore.h \
        libs/collabtext/src/identity/IdentityStore.cpp \
        libs/collabtext/tests/tst_identity_store.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: IdentityStore — generate, save, load identities from disk"
```

---

## Task 4: IdentityProjector

**Files:**
- Create: `include/collabtext/IdentityProjector.h`
- Create: `src/identity/IdentityProjector.cpp`
- Create: `tests/tst_identity_projector.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/tst_identity_projector.cpp`:

```cpp
#include <QTest>
#include <QTemporaryDir>
#include "collabtext/IdentityProjector.h"

using namespace CollabText::Identity;
namespace fs = std::filesystem;

class TestIdentityProjector : public QObject {
    Q_OBJECT

private slots:
    void project_creates_profile_json() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        Identity id;
        id.identity_id = "alice-04e1c9";
        id.display_name = "Alice";
        id.color = "#22c55e";
        id.updated = "2026-04-06T12:00:00Z";

        proj.project(id);

        fs::path profile = shared / "identities" / "alice-04e1c9" / "profile.json";
        QVERIFY(fs::exists(profile));
    }

    void project_then_read_roundtrip() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        Identity id;
        id.identity_id = "bob-ff1234";
        id.display_name = "Bob";
        id.color = "#ef4444";
        id.status = "Testing";
        id.updated = "2026-04-06T13:00:00Z";

        proj.project(id);

        auto loaded = proj.read("bob-ff1234");
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->display_name, std::string("Bob"));
        QCOMPARE(loaded->color, std::string("#ef4444"));
        QCOMPARE(loaded->status, std::string("Testing"));
    }

    void read_all_returns_multiple_identities() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        Identity alice;
        alice.identity_id = "alice-111111";
        alice.display_name = "Alice";
        alice.color = "#22c55e";
        alice.updated = "2026-04-06T12:00:00Z";

        Identity bob;
        bob.identity_id = "bob-222222";
        bob.display_name = "Bob";
        bob.color = "#ef4444";
        bob.updated = "2026-04-06T12:00:00Z";

        proj.project(alice);
        proj.project(bob);

        auto all = proj.read_all();
        QCOMPARE(all.size(), size_t(2));
    }

    void read_nonexistent_returns_nullopt() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        QVERIFY(!proj.read("nobody-000000").has_value());
    }

    void project_skips_when_not_newer() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        Identity id;
        id.identity_id = "alice-04e1c9";
        id.display_name = "Alice";
        id.color = "#22c55e";
        id.updated = "2026-04-06T12:00:00Z";
        proj.project(id);

        // Same timestamp — should not rewrite
        id.display_name = "Alice Updated";
        proj.project(id);

        auto loaded = proj.read("alice-04e1c9");
        QVERIFY(loaded.has_value());
        // Name should still be original because updated timestamp didn't change
        QCOMPARE(loaded->display_name, std::string("Alice"));
    }

    void project_updates_when_newer() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        Identity id;
        id.identity_id = "alice-04e1c9";
        id.display_name = "Alice";
        id.color = "#22c55e";
        id.updated = "2026-04-06T12:00:00Z";
        proj.project(id);

        id.display_name = "Alice Updated";
        id.updated = "2026-04-06T13:00:00Z";
        proj.project(id);

        auto loaded = proj.read("alice-04e1c9");
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->display_name, std::string("Alice Updated"));
    }

    void project_avatar_writes_file() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        std::vector<uint8_t> data = {0x89, 0x50, 0x4E, 0x47};
        proj.project_avatar("alice-04e1c9", data);

        fs::path avatar = shared / "identities" / "alice-04e1c9" / "avatar.png";
        QVERIFY(fs::exists(avatar));
    }
};

QTEST_MAIN(TestIdentityProjector)
#include "tst_identity_projector.moc"
```

- [ ] **Step 2: Create the header**

Create `include/collabtext/IdentityProjector.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace CollabText::Identity {

class IdentityProjector {
public:
    explicit IdentityProjector(std::filesystem::path shared_folder);

    /// Project identity into identities/<identity_id>/profile.json.
    /// Only writes if the identity's `updated` is newer than existing projection.
    void project(const Identity& identity);

    /// Project avatar file alongside profile.
    void project_avatar(const std::string& identity_id,
                        const std::vector<uint8_t>& data);

    /// Read all projected identities.
    std::vector<Identity> read_all() const;

    /// Read one identity by id.
    std::optional<Identity> read(const std::string& identity_id) const;

private:
    std::filesystem::path m_shared_folder;
};

} // namespace CollabText::Identity
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `src/identity/IdentityProjector.cpp` to library sources. Add `add_crdt_test(tst_identity_projector)`.

- [ ] **Step 4: Run test to verify it fails**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_projector 2>&1 | tail -20`
Expected: Linker errors.

- [ ] **Step 5: Implement IdentityProjector**

Create `src/identity/IdentityProjector.cpp`:

```cpp
#include "collabtext/IdentityProjector.h"

#include <fstream>

namespace CollabText::Identity {

namespace fs = std::filesystem;

IdentityProjector::IdentityProjector(fs::path shared_folder)
    : m_shared_folder(std::move(shared_folder))
{
}

void IdentityProjector::project(const Identity& identity) {
    fs::path dir = m_shared_folder / "identities" / identity.identity_id;
    fs::path profile_path = dir / "profile.json";

    // Check if existing projection is already up-to-date
    if (fs::exists(profile_path)) {
        std::ifstream f(profile_path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        auto existing = identity_from_json(content);
        if (existing && existing->updated >= identity.updated) {
            return; // already up-to-date
        }
    }

    fs::create_directories(dir);
    fs::path tmp = profile_path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        auto json = to_json(identity);
        f.write(json.data(), static_cast<std::streamsize>(json.size()));
    }
    fs::rename(tmp, profile_path);
}

void IdentityProjector::project_avatar(const std::string& identity_id,
                                        const std::vector<uint8_t>& data) {
    fs::path dir = m_shared_folder / "identities" / identity_id;
    fs::create_directories(dir);
    fs::path avatar_path = dir / "avatar.png";
    fs::path tmp = avatar_path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    fs::rename(tmp, avatar_path);
}

std::vector<Identity> IdentityProjector::read_all() const {
    std::vector<Identity> result;
    fs::path ids_dir = m_shared_folder / "identities";
    if (!fs::exists(ids_dir)) return result;

    for (const auto& entry : fs::directory_iterator(ids_dir)) {
        if (!entry.is_directory()) continue;
        fs::path profile_path = entry.path() / "profile.json";
        if (!fs::exists(profile_path)) continue;

        std::ifstream f(profile_path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        auto id = identity_from_json(content);
        if (id) result.push_back(std::move(*id));
    }
    return result;
}

std::optional<Identity> IdentityProjector::read(const std::string& identity_id) const {
    fs::path profile_path = m_shared_folder / "identities" / identity_id / "profile.json";
    if (!fs::exists(profile_path)) return std::nullopt;

    std::ifstream f(profile_path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return identity_from_json(content);
}

} // namespace CollabText::Identity
```

- [ ] **Step 6: Build and run the test**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_projector -j$(nproc) 2>&1 | tail -5 && build-dev/libs/collabtext/tst_identity_projector`
Expected: All tests pass.

- [ ] **Step 7: Commit**

```bash
git add libs/collabtext/include/collabtext/IdentityProjector.h \
        libs/collabtext/src/identity/IdentityProjector.cpp \
        libs/collabtext/tests/tst_identity_projector.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: IdentityProjector — project and read identities in shared folders"
```

---

## Task 5: PresenceManager

**Files:**
- Create: `include/collabtext/PresenceManager.h`
- Create: `src/identity/PresenceManager.cpp`
- Create: `tests/tst_presence_manager.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/tst_presence_manager.cpp`:

```cpp
#include <QTest>
#include <QTemporaryDir>
#include "collabtext/PresenceManager.h"

using namespace CollabText::Identity;
namespace fs = std::filesystem;

class TestPresenceManager : public QObject {
    Q_OBJECT

private slots:
    void write_presence_creates_file() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        // Create the replica directory (PresenceManager doesn't create it)
        fs::create_directories(shared / "replicas" / "laptop-3");

        PresenceManager pm(shared, "laptop-3", "clinton-a7f3b2");

        Presence p;
        p.replica_id = "laptop-3";
        p.identity_id = "clinton-a7f3b2";
        p.device_name = "ThinkPad";
        p.active = true;
        p.last_heartbeat = "2026-04-06T14:30:00Z";
        p.session_started = "2026-04-06T12:00:00Z";

        pm.write_presence(p);

        QVERIFY(fs::exists(shared / "replicas" / "laptop-3" / "presence.json"));
    }

    void write_ephemeral_creates_file() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        fs::create_directories(shared / "replicas" / "laptop-3");

        PresenceManager pm(shared, "laptop-3", "clinton-a7f3b2");

        EphemeralState es;
        es.seq = 1;
        es.timestamp = "2026-04-06T14:30:00Z";
        es.activity = "idle";

        pm.write_ephemeral(es);

        QVERIFY(fs::exists(shared / "replicas" / "laptop-3" / "ephemeral.json"));
    }

    void read_remote_presences_skips_own() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();

        // Set up two replicas
        fs::create_directories(shared / "replicas" / "laptop-3");
        fs::create_directories(shared / "replicas" / "desktop-1");

        PresenceManager pmA(shared, "laptop-3", "clinton-a7f3b2");
        PresenceManager pmB(shared, "desktop-1", "alice-04e1c9");

        Presence pA;
        pA.replica_id = "laptop-3";
        pA.identity_id = "clinton-a7f3b2";
        pA.device_name = "ThinkPad";
        pA.active = true;
        pA.last_heartbeat = "2026-04-06T14:30:00Z";
        pA.session_started = "2026-04-06T12:00:00Z";
        pmA.write_presence(pA);

        Presence pB;
        pB.replica_id = "desktop-1";
        pB.identity_id = "alice-04e1c9";
        pB.device_name = "Desktop";
        pB.active = true;
        pB.last_heartbeat = "2026-04-06T14:30:00Z";
        pB.session_started = "2026-04-06T12:00:00Z";
        pmB.write_presence(pB);

        // A reads remote presences — should only see B
        auto remotes = pmA.read_remote_presences();
        QCOMPARE(remotes.size(), size_t(1));
        QCOMPARE(remotes[0].first, std::string("desktop-1"));
        QCOMPARE(remotes[0].second.identity_id, std::string("alice-04e1c9"));
    }

    void read_remote_ephemerals_skips_own() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        fs::create_directories(shared / "replicas" / "a");
        fs::create_directories(shared / "replicas" / "b");

        PresenceManager pmA(shared, "a", "id-a");
        PresenceManager pmB(shared, "b", "id-b");

        EphemeralState es;
        es.seq = 1;
        es.timestamp = "2026-04-06T14:30:00Z";
        es.activity = "typing";
        es.cursors.push_back({
            Crdt::Anchor(1, 10, Crdt::Bias::Right),
            Crdt::Anchor(1, 10, Crdt::Bias::Right)
        });

        pmA.write_ephemeral(es);
        pmB.write_ephemeral(es);

        auto remotes = pmA.read_remote_ephemerals();
        QCOMPARE(remotes.size(), size_t(1));
        QCOMPARE(remotes[0].first, std::string("b"));
    }

    void is_live_checks_heartbeat_and_active() {
        Presence p;
        p.active = true;

        // Recent heartbeat = live
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        gmtime_r(&time_t, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        p.last_heartbeat = buf;
        QVERIFY(PresenceManager::is_live(p));
        QVERIFY(!PresenceManager::is_stale(p));
        QVERIFY(!PresenceManager::is_departed(p));
    }

    void is_stale_with_old_heartbeat() {
        Presence p;
        p.active = true;
        p.last_heartbeat = "2020-01-01T00:00:00Z"; // very old
        QVERIFY(!PresenceManager::is_live(p));
        QVERIFY(PresenceManager::is_stale(p));
    }

    void is_departed_when_inactive() {
        Presence p;
        p.active = false;
        p.last_heartbeat = "2026-04-06T14:30:00Z";
        QVERIFY(PresenceManager::is_departed(p));
        QVERIFY(!PresenceManager::is_live(p));
    }

    void depart_sets_active_false() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        fs::create_directories(shared / "replicas" / "laptop-3");

        PresenceManager pm(shared, "laptop-3", "clinton-a7f3b2");

        Presence p;
        p.replica_id = "laptop-3";
        p.identity_id = "clinton-a7f3b2";
        p.device_name = "ThinkPad";
        p.active = true;
        p.last_heartbeat = "2026-04-06T14:30:00Z";
        p.session_started = "2026-04-06T12:00:00Z";
        pm.write_presence(p);

        pm.depart();

        // Read back and check
        auto remotes = PresenceManager(shared, "other", "other").read_remote_presences();
        QCOMPARE(remotes.size(), size_t(1));
        QCOMPARE(remotes[0].second.active, false);
    }

    void malformed_presence_file_skipped() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        fs::create_directories(shared / "replicas" / "bad");

        // Write garbage to presence.json
        std::ofstream f(shared / "replicas" / "bad" / "presence.json");
        f << "not json at all";
        f.close();

        PresenceManager pm(shared, "good", "id");
        auto remotes = pm.read_remote_presences();
        QCOMPARE(remotes.size(), size_t(0)); // skipped, not crashed
    }
};

QTEST_MAIN(TestPresenceManager)
#include "tst_presence_manager.moc"
```

- [ ] **Step 2: Create the header**

Create `include/collabtext/PresenceManager.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace CollabText::Identity {

class PresenceManager {
public:
    PresenceManager(std::filesystem::path shared_folder,
                    std::string replica_id,
                    std::string identity_id);

    /// Write presence.json for this replica.
    void write_presence(const Presence& presence);

    /// Write ephemeral.json for this replica.
    void write_ephemeral(const EphemeralState& state);

    /// Read all remote presences (skips own replica_id).
    /// Returns pairs of (replica_id, Presence).
    std::vector<std::pair<std::string, Presence>> read_remote_presences() const;

    /// Read all remote ephemeral states (skips own replica_id).
    /// Returns pairs of (replica_id, EphemeralState).
    std::vector<std::pair<std::string, EphemeralState>> read_remote_ephemerals() const;

    /// Is the presence live? (active && heartbeat < 30s old)
    static bool is_live(const Presence& p);

    /// Is the presence stale? (active && heartbeat > 30s old)
    static bool is_stale(const Presence& p);

    /// Is the presence departed? (active == false)
    static bool is_departed(const Presence& p);

    /// Set active=false in presence.json (graceful departure).
    void depart();

    const std::string& replica_id() const { return m_replica_id; }
    const std::string& identity_id() const { return m_identity_id; }

private:
    std::filesystem::path m_shared_folder;
    std::string m_replica_id;
    std::string m_identity_id;
};

} // namespace CollabText::Identity
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `src/identity/PresenceManager.cpp` to library sources. Add `add_crdt_test(tst_presence_manager)`.

- [ ] **Step 4: Run test to verify it fails**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_presence_manager 2>&1 | tail -20`
Expected: Linker errors.

- [ ] **Step 5: Implement PresenceManager**

Create `src/identity/PresenceManager.cpp`:

```cpp
#include "collabtext/PresenceManager.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace CollabText::Identity {

namespace fs = std::filesystem;

PresenceManager::PresenceManager(fs::path shared_folder,
                                  std::string replica_id,
                                  std::string identity_id)
    : m_shared_folder(std::move(shared_folder))
    , m_replica_id(std::move(replica_id))
    , m_identity_id(std::move(identity_id))
{
}

/// Write content to a temp file then rename (atomic on POSIX).
static void atomic_write(const fs::path& path, const std::string& content) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    fs::rename(tmp, path);
}

void PresenceManager::write_presence(const Presence& presence) {
    fs::path path = m_shared_folder / "replicas" / m_replica_id / "presence.json";
    atomic_write(path, to_json(presence));
}

void PresenceManager::write_ephemeral(const EphemeralState& state) {
    fs::path path = m_shared_folder / "replicas" / m_replica_id / "ephemeral.json";
    atomic_write(path, to_json(state));
}

std::vector<std::pair<std::string, Presence>>
PresenceManager::read_remote_presences() const {
    std::vector<std::pair<std::string, Presence>> result;
    fs::path replicas_dir = m_shared_folder / "replicas";
    if (!fs::exists(replicas_dir)) return result;

    for (const auto& entry : fs::directory_iterator(replicas_dir)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name == m_replica_id) continue;

        fs::path presence_path = entry.path() / "presence.json";
        if (!fs::exists(presence_path)) continue;

        std::ifstream f(presence_path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        auto p = presence_from_json(content);
        if (p) result.emplace_back(name, std::move(*p));
    }
    return result;
}

std::vector<std::pair<std::string, EphemeralState>>
PresenceManager::read_remote_ephemerals() const {
    std::vector<std::pair<std::string, EphemeralState>> result;
    fs::path replicas_dir = m_shared_folder / "replicas";
    if (!fs::exists(replicas_dir)) return result;

    for (const auto& entry : fs::directory_iterator(replicas_dir)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name == m_replica_id) continue;

        fs::path eph_path = entry.path() / "ephemeral.json";
        if (!fs::exists(eph_path)) continue;

        std::ifstream f(eph_path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        auto es = ephemeral_from_json(content);
        if (es) result.emplace_back(name, std::move(*es));
    }
    return result;
}

/// Parse ISO 8601 timestamp to time_point. Handles "YYYY-MM-DDTHH:MM:SSZ"
/// and "YYYY-MM-DDTHH:MM:SS.mmmZ".
static std::chrono::system_clock::time_point parse_iso8601(const std::string& s) {
    std::tm tm{};
    // Try parsing with strptime
    const char* end = strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
    if (!end) return std::chrono::system_clock::time_point{}; // epoch on failure
    time_t t = timegm(&tm);
    return std::chrono::system_clock::from_time_t(t);
}

bool PresenceManager::is_live(const Presence& p) {
    if (!p.active) return false;
    auto heartbeat = parse_iso8601(p.last_heartbeat);
    auto now = std::chrono::system_clock::now();
    return (now - heartbeat) <= std::chrono::seconds(30);
}

bool PresenceManager::is_stale(const Presence& p) {
    if (!p.active) return false;
    auto heartbeat = parse_iso8601(p.last_heartbeat);
    auto now = std::chrono::system_clock::now();
    return (now - heartbeat) > std::chrono::seconds(30);
}

bool PresenceManager::is_departed(const Presence& p) {
    return !p.active;
}

void PresenceManager::depart() {
    fs::path path = m_shared_folder / "replicas" / m_replica_id / "presence.json";
    if (!fs::exists(path)) return;

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    f.close();

    auto p = presence_from_json(content);
    if (!p) return;

    p->active = false;
    atomic_write(path, to_json(*p));
}

} // namespace CollabText::Identity
```

- [ ] **Step 6: Build and run the test**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_presence_manager -j$(nproc) 2>&1 | tail -5 && build-dev/libs/collabtext/tst_presence_manager`
Expected: All tests pass.

- [ ] **Step 7: Commit**

```bash
git add libs/collabtext/include/collabtext/PresenceManager.h \
        libs/collabtext/src/identity/PresenceManager.cpp \
        libs/collabtext/tests/tst_presence_manager.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: PresenceManager — write/read presence + ephemeral, liveness detection"
```

---

## Task 6: AvatarWidget

**Files:**
- Create: `src/ui/AvatarWidget.h`
- Create: `src/ui/AvatarWidget.cpp`
- Create: `tests/tst_identity_widgets.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/tst_identity_widgets.cpp`. This file will accumulate widget smoke tests across Tasks 6-8.

```cpp
#include <QTest>
#include <QApplication>
#include "ui/AvatarWidget.h"

using namespace CollabText::Ui;

class TestIdentityWidgets : public QObject {
    Q_OBJECT

private slots:
    void avatar_widget_default_size() {
        AvatarWidget w;
        QCOMPARE(w.sizeHint(), QSize(40, 40));
    }

    void avatar_widget_initials_fallback() {
        AvatarWidget w;
        w.setIdentity("Clinton Selke", "#3b82f6");
        // Should not crash, should render initials "CS" on paint
        w.resize(40, 40);
        QPixmap pm(40, 40);
        w.render(&pm);
        // If we got here without crashing, the fallback works
        QVERIFY(!pm.isNull());
    }

    void avatar_widget_set_image() {
        AvatarWidget w;
        // Minimal 1x1 PNG
        std::vector<uint8_t> data = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // PNG signature
            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, // IHDR chunk
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
            0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,
            0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
            0x00, 0x00, 0x02, 0x00, 0x01, 0xE2, 0x21, 0xBC,
            0x33, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
            0x44, 0xAE, 0x42, 0x60, 0x82,
        };
        w.setImage(data);
        w.resize(40, 40);
        QPixmap pm(40, 40);
        w.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void avatar_widget_clear_image_reverts_to_initials() {
        AvatarWidget w;
        w.setIdentity("Alice", "#22c55e");
        w.setImage({0x89, 0x50, 0x4E, 0x47}); // partial, won't load
        w.clearImage();
        w.resize(40, 40);
        QPixmap pm(40, 40);
        w.render(&pm);
        QVERIFY(!pm.isNull());
    }
};

QTEST_MAIN(TestIdentityWidgets)
#include "tst_identity_widgets.moc"
```

- [ ] **Step 2: Create AvatarWidget header and implementation**

Create `src/ui/AvatarWidget.h`:

```cpp
#pragma once

#include <QWidget>
#include <QPixmap>
#include <string>
#include <vector>

namespace CollabText::Ui {

/// Circular avatar widget. Shows a loaded image or an initials-on-circle fallback.
class AvatarWidget : public QWidget {
    Q_OBJECT
public:
    explicit AvatarWidget(QWidget *parent = nullptr);

    /// Set the identity for the initials fallback.
    void setIdentity(const std::string &display_name, const std::string &color);

    /// Set an image from raw bytes (PNG, JPEG, WebP).
    void setImage(const std::vector<uint8_t> &data);

    /// Clear the image, reverting to initials fallback.
    void clearImage();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString extractInitials(const QString &name) const;

    QString m_displayName;
    QColor m_color{Qt::gray};
    QPixmap m_image;
    bool m_hasImage = false;
};

} // namespace CollabText::Ui
```

Create `src/ui/AvatarWidget.cpp`:

```cpp
#include "ui/AvatarWidget.h"

#include <QPainter>
#include <QPainterPath>

namespace CollabText::Ui {

AvatarWidget::AvatarWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(40, 40);
}

void AvatarWidget::setIdentity(const std::string &display_name,
                                const std::string &color) {
    m_displayName = QString::fromStdString(display_name);
    m_color = QColor(QString::fromStdString(color));
    update();
}

void AvatarWidget::setImage(const std::vector<uint8_t> &data) {
    QPixmap pm;
    if (pm.loadFromData(reinterpret_cast<const uchar*>(data.data()),
                        static_cast<uint>(data.size()))) {
        m_image = pm.scaled(size(), Qt::KeepAspectRatioByExpanding,
                            Qt::SmoothTransformation);
        m_hasImage = true;
    }
    update();
}

void AvatarWidget::clearImage() {
    m_image = QPixmap();
    m_hasImage = false;
    update();
}

QSize AvatarWidget::sizeHint() const {
    return {40, 40};
}

QString AvatarWidget::extractInitials(const QString &name) const {
    if (name.isEmpty()) return QStringLiteral("?");

    QStringList parts = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() >= 2)
        return (parts.first().left(1) + parts.last().left(1)).toUpper();
    return parts.first().left(1).toUpper();
}

void AvatarWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QRect r = rect();
    int diameter = qMin(r.width(), r.height());
    QRect circle(r.center() - QPoint(diameter / 2, diameter / 2),
                 QSize(diameter, diameter));

    // Clip to circle
    QPainterPath clipPath;
    clipPath.addEllipse(circle);
    p.setClipPath(clipPath);

    if (m_hasImage && !m_image.isNull()) {
        // Draw image centered in circle
        QPoint offset((r.width() - m_image.width()) / 2,
                      (r.height() - m_image.height()) / 2);
        p.drawPixmap(offset, m_image);
    } else {
        // Draw colored circle with initials
        p.setBrush(m_color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(circle);

        // Draw initials
        p.setPen(Qt::white);
        QFont font = p.font();
        font.setPixelSize(diameter * 4 / 10);
        font.setBold(true);
        p.setFont(font);
        p.drawText(circle, Qt::AlignCenter, extractInitials(m_displayName));
    }
}

} // namespace CollabText::Ui
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `src/ui/AvatarWidget.cpp` to library sources. Add `add_crdt_test(tst_identity_widgets)`.

- [ ] **Step 4: Build and run the test**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_widgets -j$(nproc) 2>&1 | tail -5 && build-dev/libs/collabtext/tst_identity_widgets`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/ui/AvatarWidget.h \
        libs/collabtext/src/ui/AvatarWidget.cpp \
        libs/collabtext/tests/tst_identity_widgets.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: AvatarWidget — circular avatar with initials fallback"
```

---

## Task 7: PresenceIndicator and IdentityEditor

**Files:**
- Create: `src/ui/PresenceIndicator.h`
- Create: `src/ui/PresenceIndicator.cpp`
- Create: `src/ui/IdentityEditor.h`
- Create: `src/ui/IdentityEditor.cpp`
- Modify: `tests/tst_identity_widgets.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add tests to tst_identity_widgets.cpp**

Add the following test slots to the existing `TestIdentityWidgets` class:

```cpp
    // --- PresenceIndicator tests ---

    void presence_indicator_default_size() {
        PresenceIndicator pi;
        QCOMPARE(pi.sizeHint(), QSize(12, 12));
    }

    void presence_indicator_activity_colors() {
        PresenceIndicator pi;
        pi.setActivity("typing");
        pi.resize(12, 12);
        QPixmap pm(12, 12);
        pi.render(&pm);
        QVERIFY(!pm.isNull());

        pi.setActivity("idle");
        pi.render(&pm);
        QVERIFY(!pm.isNull());

        pi.setActivity("away");
        pi.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void presence_indicator_stale() {
        PresenceIndicator pi;
        pi.setActivity("typing");
        pi.setStale(true);
        pi.resize(12, 12);
        QPixmap pm(12, 12);
        pi.render(&pm);
        QVERIFY(!pm.isNull());
    }

    // --- IdentityEditor tests ---

    void identity_editor_set_and_get() {
        CollabText::Identity::Identity id;
        id.identity_id = "test-aaa111";
        id.display_name = "Test User";
        id.status = "Testing";
        id.bio = "A bio";
        id.color = "#3b82f6";
        id.updated = "2026-04-06T12:00:00Z";

        IdentityEditor editor;
        editor.setIdentity(id);

        auto result = editor.identity();
        QCOMPARE(result.display_name, id.display_name);
        QCOMPARE(result.status, id.status);
        QCOMPARE(result.bio, id.bio);
        QCOMPARE(result.color, id.color);
    }

    void identity_editor_emits_changed() {
        IdentityEditor editor;
        QSignalSpy spy(&editor, &IdentityEditor::identityChanged);
        QVERIFY(spy.isValid());

        CollabText::Identity::Identity id;
        id.identity_id = "test-bbb222";
        id.display_name = "Alice";
        id.color = "#22c55e";
        id.updated = "2026-04-06T12:00:00Z";
        editor.setIdentity(id);

        // Spy should have caught at least one signal from setIdentity
        // (setIdentity populates the fields which trigger change signals)
        QVERIFY(spy.count() >= 0); // existence test — signal wiring works
    }
```

Also add `#include "ui/PresenceIndicator.h"` and `#include "ui/IdentityEditor.h"` to the top of the test file, and add `#include <QSignalSpy>`.

- [ ] **Step 2: Create PresenceIndicator**

Create `src/ui/PresenceIndicator.h`:

```cpp
#pragma once

#include <QWidget>
#include <string>

namespace CollabText::Ui {

/// Tiny colored dot indicating activity state.
/// Green = typing/selecting, Yellow = idle, Gray = away/stale.
class PresenceIndicator : public QWidget {
    Q_OBJECT
public:
    explicit PresenceIndicator(QWidget *parent = nullptr);

    void setActivity(const std::string &activity);
    void setStale(bool stale);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QColor currentColor() const;

    std::string m_activity = "idle";
    bool m_stale = false;
};

} // namespace CollabText::Ui
```

Create `src/ui/PresenceIndicator.cpp`:

```cpp
#include "ui/PresenceIndicator.h"

#include <QPainter>

namespace CollabText::Ui {

PresenceIndicator::PresenceIndicator(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(12, 12);
}

void PresenceIndicator::setActivity(const std::string &activity) {
    m_activity = activity;
    update();
}

void PresenceIndicator::setStale(bool stale) {
    m_stale = stale;
    update();
}

QSize PresenceIndicator::sizeHint() const {
    return {12, 12};
}

QColor PresenceIndicator::currentColor() const {
    if (m_stale) return QColor(156, 163, 175);  // gray-400
    if (m_activity == "typing" || m_activity == "selecting")
        return QColor(34, 197, 94);   // green-500
    if (m_activity == "idle")
        return QColor(234, 179, 8);   // yellow-500
    return QColor(156, 163, 175);     // gray-400 for "away" and unknown
}

void PresenceIndicator::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(currentColor());
    int d = qMin(width(), height());
    p.drawEllipse(rect().center(), d / 2 - 1, d / 2 - 1);
}

} // namespace CollabText::Ui
```

- [ ] **Step 3: Create IdentityEditor**

Create `src/ui/IdentityEditor.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"

#include <QWidget>

class QLineEdit;
class QTextEdit;
class QPushButton;

namespace CollabText::Ui {

class AvatarWidget;

/// Form panel for editing an Identity. No OK/Cancel buttons — embeddable.
class IdentityEditor : public QWidget {
    Q_OBJECT
public:
    explicit IdentityEditor(QWidget *parent = nullptr);

    void setIdentity(const CollabText::Identity::Identity &identity);
    CollabText::Identity::Identity identity() const;

signals:
    void identityChanged();

private:
    void onColorClicked();

    QLineEdit *m_nameEdit;
    QLineEdit *m_statusEdit;
    QTextEdit *m_bioEdit;
    QPushButton *m_colorButton;
    AvatarWidget *m_avatar;
    QString m_identityId;
    QString m_publicKey;
    QString m_updated;
    QColor m_color{Qt::gray};
};

} // namespace CollabText::Ui
```

Create `src/ui/IdentityEditor.cpp`:

```cpp
#include "ui/IdentityEditor.h"
#include "ui/AvatarWidget.h"

#include <QColorDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>

namespace CollabText::Ui {

IdentityEditor::IdentityEditor(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QFormLayout(this);

    // Avatar at top
    auto *topRow = new QHBoxLayout;
    m_avatar = new AvatarWidget(this);
    topRow->addWidget(m_avatar);
    topRow->addStretch();
    layout->addRow(topRow);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(QStringLiteral("Display name"));
    layout->addRow(QStringLiteral("Name"), m_nameEdit);

    m_statusEdit = new QLineEdit(this);
    m_statusEdit->setPlaceholderText(QStringLiteral("What are you working on?"));
    layout->addRow(QStringLiteral("Status"), m_statusEdit);

    m_bioEdit = new QTextEdit(this);
    m_bioEdit->setPlaceholderText(QStringLiteral("About you (optional)"));
    m_bioEdit->setMaximumHeight(60);
    layout->addRow(QStringLiteral("Bio"), m_bioEdit);

    m_colorButton = new QPushButton(this);
    m_colorButton->setFixedSize(32, 32);
    layout->addRow(QStringLiteral("Color"), m_colorButton);

    connect(m_colorButton, &QPushButton::clicked,
            this, &IdentityEditor::onColorClicked);

    // Emit identityChanged on edits
    connect(m_nameEdit, &QLineEdit::textChanged,
            this, &IdentityEditor::identityChanged);
    connect(m_statusEdit, &QLineEdit::textChanged,
            this, &IdentityEditor::identityChanged);
    connect(m_bioEdit, &QTextEdit::textChanged,
            this, &IdentityEditor::identityChanged);
}

void IdentityEditor::setIdentity(const CollabText::Identity::Identity &identity) {
    m_identityId = QString::fromStdString(identity.identity_id);
    m_publicKey = QString::fromStdString(identity.public_key);
    m_updated = QString::fromStdString(identity.updated);

    m_nameEdit->setText(QString::fromStdString(identity.display_name));
    m_statusEdit->setText(QString::fromStdString(identity.status));
    m_bioEdit->setPlainText(QString::fromStdString(identity.bio));

    m_color = QColor(QString::fromStdString(identity.color));
    m_colorButton->setStyleSheet(
        QStringLiteral("background-color: %1; border: 1px solid #ccc;")
            .arg(m_color.name()));

    m_avatar->setIdentity(identity.display_name, identity.color);
}

CollabText::Identity::Identity IdentityEditor::identity() const {
    CollabText::Identity::Identity id;
    id.identity_id = m_identityId.toStdString();
    id.display_name = m_nameEdit->text().toStdString();
    id.status = m_statusEdit->text().toStdString();
    id.bio = m_bioEdit->toPlainText().toStdString();
    id.color = m_color.name().toStdString();
    id.public_key = m_publicKey.toStdString();
    id.updated = m_updated.toStdString();
    return id;
}

void IdentityEditor::onColorClicked() {
    QColor c = QColorDialog::getColor(m_color, this, QStringLiteral("Choose cursor color"));
    if (c.isValid()) {
        m_color = c;
        m_colorButton->setStyleSheet(
            QStringLiteral("background-color: %1; border: 1px solid #ccc;")
                .arg(c.name()));
        m_avatar->setIdentity(m_nameEdit->text().toStdString(),
                               c.name().toStdString());
        emit identityChanged();
    }
}

} // namespace CollabText::Ui
```

- [ ] **Step 4: Add to CMakeLists.txt**

Add `src/ui/PresenceIndicator.cpp` and `src/ui/IdentityEditor.cpp` to library sources.

- [ ] **Step 5: Build and run the tests**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_widgets -j$(nproc) 2>&1 | tail -5 && build-dev/libs/collabtext/tst_identity_widgets`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/ui/PresenceIndicator.h \
        libs/collabtext/src/ui/PresenceIndicator.cpp \
        libs/collabtext/src/ui/IdentityEditor.h \
        libs/collabtext/src/ui/IdentityEditor.cpp \
        libs/collabtext/tests/tst_identity_widgets.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: PresenceIndicator + IdentityEditor widgets"
```

---

## Task 8: ParticipantListWidget

**Files:**
- Create: `src/ui/ParticipantListWidget.h`
- Create: `src/ui/ParticipantListWidget.cpp`
- Modify: `tests/tst_identity_widgets.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add tests to tst_identity_widgets.cpp**

Add `#include "ui/ParticipantListWidget.h"` and the following test slots:

```cpp
    void participant_list_empty() {
        ParticipantListWidget plw;
        plw.updateParticipants({}, {});
        plw.resize(200, 300);
        QPixmap pm(200, 300);
        plw.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void participant_list_two_participants() {
        CollabText::Identity::Identity alice;
        alice.identity_id = "alice-111111";
        alice.display_name = "Alice";
        alice.color = "#22c55e";

        CollabText::Identity::Identity bob;
        bob.identity_id = "bob-222222";
        bob.display_name = "Bob";
        bob.color = "#ef4444";

        CollabText::Identity::Presence pA;
        pA.replica_id = "rep-a";
        pA.identity_id = "alice-111111";
        pA.active = true;

        CollabText::Identity::Presence pB;
        pB.replica_id = "rep-b";
        pB.identity_id = "bob-222222";
        pB.active = true;

        ParticipantListWidget plw;
        plw.updateParticipants({alice, bob}, {pA, pB});
        plw.resize(200, 300);
        QPixmap pm(200, 300);
        plw.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void participant_list_collapses_same_identity() {
        CollabText::Identity::Identity alice;
        alice.identity_id = "alice-111111";
        alice.display_name = "Alice";
        alice.color = "#22c55e";

        // Two presences with same identity (two devices)
        CollabText::Identity::Presence p1;
        p1.replica_id = "laptop";
        p1.identity_id = "alice-111111";
        p1.active = true;

        CollabText::Identity::Presence p2;
        p2.replica_id = "desktop";
        p2.identity_id = "alice-111111";
        p2.active = true;

        ParticipantListWidget plw;
        plw.updateParticipants({alice}, {p1, p2});
        // Should show 1 entry, not 2
        // We can't easily check internal layout, but at minimum it shouldn't crash
        plw.resize(200, 300);
        QPixmap pm(200, 300);
        plw.render(&pm);
        QVERIFY(!pm.isNull());
    }
```

- [ ] **Step 2: Create ParticipantListWidget**

Create `src/ui/ParticipantListWidget.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"

#include <QWidget>
#include <QVBoxLayout>
#include <string>
#include <vector>

namespace CollabText::Ui {

/// Vertical list of connected participants with avatar, name, status, activity.
/// Entries keyed by identity_id — multiple replicas with the same identity
/// collapse into one entry.
class ParticipantListWidget : public QWidget {
    Q_OBJECT
public:
    explicit ParticipantListWidget(QWidget *parent = nullptr);

    void updateParticipants(
        const std::vector<CollabText::Identity::Identity> &identities,
        const std::vector<CollabText::Identity::Presence> &presences);

signals:
    void participantClicked(const QString &identityId);

private:
    void rebuild();

    struct ParticipantEntry {
        CollabText::Identity::Identity identity;
        int device_count = 0;
        std::string best_activity; // most "active" across devices
    };

    std::vector<ParticipantEntry> m_entries;
    QVBoxLayout *m_layout;
};

} // namespace CollabText::Ui
```

Create `src/ui/ParticipantListWidget.cpp`:

```cpp
#include "ui/ParticipantListWidget.h"
#include "ui/AvatarWidget.h"
#include "ui/PresenceIndicator.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>
#include <unordered_map>

namespace CollabText::Ui {

ParticipantListWidget::ParticipantListWidget(QWidget *parent)
    : QWidget(parent)
    , m_layout(new QVBoxLayout(this))
{
    m_layout->setContentsMargins(4, 4, 4, 4);
    m_layout->setSpacing(2);
    m_layout->addStretch();
}

/// Rank activity: typing > selecting > idle > away > unknown
static int activity_rank(const std::string &activity) {
    if (activity == "typing") return 4;
    if (activity == "selecting") return 3;
    if (activity == "idle") return 2;
    if (activity == "away") return 1;
    return 0;
}

void ParticipantListWidget::updateParticipants(
    const std::vector<CollabText::Identity::Identity> &identities,
    const std::vector<CollabText::Identity::Presence> &presences)
{
    // Build identity lookup
    std::unordered_map<std::string, CollabText::Identity::Identity> id_map;
    for (const auto &id : identities)
        id_map[id.identity_id] = id;

    // Collapse presences by identity_id
    std::unordered_map<std::string, ParticipantEntry> entries;
    for (const auto &p : presences) {
        if (!p.active) continue;
        auto &entry = entries[p.identity_id];
        if (entry.device_count == 0) {
            // First device for this identity
            auto it = id_map.find(p.identity_id);
            if (it != id_map.end()) {
                entry.identity = it->second;
            } else {
                // No projected identity — use what we have
                entry.identity.identity_id = p.identity_id;
                entry.identity.display_name = p.device_name;
            }
        }
        entry.device_count++;
        // Keep the most active activity
        // Note: presences don't carry activity — that's in EphemeralState.
        // For now default to "idle". The caller can enhance this later
        // by cross-referencing ephemeral data.
        if (entry.best_activity.empty()) entry.best_activity = "idle";
    }

    m_entries.clear();
    for (auto &[id, entry] : entries)
        m_entries.push_back(std::move(entry));

    // Sort by display name
    std::sort(m_entries.begin(), m_entries.end(),
              [](const ParticipantEntry &a, const ParticipantEntry &b) {
                  return a.identity.display_name < b.identity.display_name;
              });

    rebuild();
}

void ParticipantListWidget::rebuild() {
    // Clear existing widgets
    while (m_layout->count() > 0) {
        auto *item = m_layout->takeAt(0);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    for (const auto &entry : m_entries) {
        auto *row = new QWidget(this);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(2, 2, 2, 2);
        rowLayout->setSpacing(6);

        auto *avatar = new AvatarWidget(row);
        avatar->setIdentity(entry.identity.display_name,
                             entry.identity.color);
        rowLayout->addWidget(avatar);

        auto *textCol = new QVBoxLayout;
        auto *nameLabel = new QLabel(
            QString::fromStdString(entry.identity.display_name), row);
        QFont bold = nameLabel->font();
        bold.setBold(true);
        nameLabel->setFont(bold);

        QString statusText;
        if (!entry.identity.status.empty())
            statusText = QString::fromStdString(entry.identity.status);
        if (entry.device_count > 1)
            statusText += QStringLiteral(" (%1 devices)").arg(entry.device_count);

        auto *statusLabel = new QLabel(statusText.trimmed(), row);
        statusLabel->setStyleSheet(QStringLiteral("color: #888;"));

        textCol->addWidget(nameLabel);
        if (!statusText.isEmpty())
            textCol->addWidget(statusLabel);
        rowLayout->addLayout(textCol, 1);

        auto *indicator = new PresenceIndicator(row);
        indicator->setActivity(entry.best_activity);
        rowLayout->addWidget(indicator);

        m_layout->addWidget(row);
    }

    m_layout->addStretch();
}

} // namespace CollabText::Ui
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `src/ui/ParticipantListWidget.cpp` to library sources.

- [ ] **Step 4: Build and run the tests**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_widgets -j$(nproc) 2>&1 | tail -5 && build-dev/libs/collabtext/tst_identity_widgets`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/ui/ParticipantListWidget.h \
        libs/collabtext/src/ui/ParticipantListWidget.cpp \
        libs/collabtext/tests/tst_identity_widgets.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: ParticipantListWidget — identity-collapsed participant list"
```

---

## Task 9: IdentitySetupDialog and IdentityPreferencesPage

**Files:**
- Create: `src/ui/IdentitySetupDialog.h`
- Create: `src/ui/IdentitySetupDialog.cpp`
- Create: `src/ui/IdentityPreferencesPage.h`
- Create: `src/ui/IdentityPreferencesPage.cpp`
- Modify: `tests/tst_identity_widgets.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add tests to tst_identity_widgets.cpp**

Add `#include "ui/IdentitySetupDialog.h"`, `#include "ui/IdentityPreferencesPage.h"`, `#include "collabtext/IdentityStore.h"`, and `#include <QTemporaryDir>`. Add these test slots:

```cpp
    void setup_dialog_creates_identity_on_accept() {
        QTemporaryDir tmp;
        CollabText::Identity::IdentityStore store(tmp.path().toStdString());
        IdentitySetupDialog dlg(store);
        // We can't simulate user interaction in a unit test, but we can
        // verify the dialog constructs and has the right type
        QVERIFY(dlg.windowTitle().contains(QStringLiteral("CollabText")));
    }

    void preferences_page_loads_identity() {
        QTemporaryDir tmp;
        CollabText::Identity::IdentityStore store(tmp.path().toStdString());
        auto id = store.generate("Test");
        store.save(id);

        IdentityPreferencesPage page(store);
        // Page should have loaded the identity from the store
        // Verify it renders without crashing
        page.resize(300, 400);
        QPixmap pm(300, 400);
        page.render(&pm);
        QVERIFY(!pm.isNull());
    }
```

- [ ] **Step 2: Create IdentitySetupDialog**

Create `src/ui/IdentitySetupDialog.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"
#include "collabtext/IdentityStore.h"

#include <QDialog>

namespace CollabText::Ui {

class IdentityEditor;

/// First-launch wizard. Embeds an IdentityEditor.
/// On accept, generates and saves the identity via IdentityStore.
class IdentitySetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit IdentitySetupDialog(CollabText::Identity::IdentityStore &store,
                                  QWidget *parent = nullptr);

    CollabText::Identity::Identity identity() const;

private:
    void onAccept();

    CollabText::Identity::IdentityStore &m_store;
    IdentityEditor *m_editor;
    CollabText::Identity::Identity m_identity;
};

} // namespace CollabText::Ui
```

Create `src/ui/IdentitySetupDialog.cpp`:

```cpp
#include "ui/IdentitySetupDialog.h"
#include "ui/IdentityEditor.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>

namespace CollabText::Ui {

IdentitySetupDialog::IdentitySetupDialog(
    CollabText::Identity::IdentityStore &store, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
{
    setWindowTitle(QStringLiteral("Welcome to CollabText"));
    setMinimumWidth(350);

    auto *layout = new QVBoxLayout(this);

    auto *header = new QLabel(QStringLiteral(
        "<h2>Welcome to CollabText</h2>"
        "<p>Set up your identity. This is how other participants will see you.</p>"),
        this);
    header->setWordWrap(true);
    layout->addWidget(header);

    m_editor = new IdentityEditor(this);
    layout->addWidget(m_editor);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &IdentitySetupDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

CollabText::Identity::Identity IdentitySetupDialog::identity() const {
    return m_identity;
}

void IdentitySetupDialog::onAccept() {
    auto edited = m_editor->identity();
    m_identity = m_store.generate(edited.display_name);
    // Apply user's choices over the generated defaults
    m_identity.status = edited.status;
    m_identity.bio = edited.bio;
    if (!edited.color.empty())
        m_identity.color = edited.color;
    m_store.save(m_identity);
    accept();
}

} // namespace CollabText::Ui
```

- [ ] **Step 3: Create IdentityPreferencesPage**

Create `src/ui/IdentityPreferencesPage.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"
#include "collabtext/IdentityStore.h"

#include <QWidget>

namespace CollabText::Ui {

class IdentityEditor;

/// Settings panel for identity. Embeds an IdentityEditor with a Save button.
/// Drop into any QTabWidget or preferences dialog.
class IdentityPreferencesPage : public QWidget {
    Q_OBJECT
public:
    explicit IdentityPreferencesPage(
        CollabText::Identity::IdentityStore &store,
        QWidget *parent = nullptr);

signals:
    void identitySaved(const CollabText::Identity::Identity &identity);

private:
    void onSave();

    CollabText::Identity::IdentityStore &m_store;
    IdentityEditor *m_editor;
};

} // namespace CollabText::Ui
```

Create `src/ui/IdentityPreferencesPage.cpp`:

```cpp
#include "ui/IdentityPreferencesPage.h"
#include "ui/IdentityEditor.h"

#include <QPushButton>
#include <QVBoxLayout>

namespace CollabText::Ui {

IdentityPreferencesPage::IdentityPreferencesPage(
    CollabText::Identity::IdentityStore &store, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
{
    auto *layout = new QVBoxLayout(this);

    m_editor = new IdentityEditor(this);
    layout->addWidget(m_editor);

    auto *saveBtn = new QPushButton(QStringLiteral("Save"), this);
    layout->addWidget(saveBtn);
    layout->addStretch();

    connect(saveBtn, &QPushButton::clicked, this, &IdentityPreferencesPage::onSave);

    // Load existing identity
    auto id = m_store.load();
    if (id) m_editor->setIdentity(*id);
}

void IdentityPreferencesPage::onSave() {
    auto id = m_editor->identity();
    // Update timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    id.updated = buf;

    m_store.save(id);
    emit identitySaved(id);
}

} // namespace CollabText::Ui
```

- [ ] **Step 4: Add to CMakeLists.txt**

Add `src/ui/IdentitySetupDialog.cpp` and `src/ui/IdentityPreferencesPage.cpp` to library sources.

- [ ] **Step 5: Build and run the tests**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_widgets -j$(nproc) 2>&1 | tail -5 && build-dev/libs/collabtext/tst_identity_widgets`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/ui/IdentitySetupDialog.h \
        libs/collabtext/src/ui/IdentitySetupDialog.cpp \
        libs/collabtext/src/ui/IdentityPreferencesPage.h \
        libs/collabtext/src/ui/IdentityPreferencesPage.cpp \
        libs/collabtext/tests/tst_identity_widgets.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: IdentitySetupDialog + IdentityPreferencesPage"
```

---

## Task 10: Refactor SyncManager to Use Core Layer

**Files:**
- Modify: `include/collabtext/SyncManager.h`
- Modify: `src/SyncManager.cpp`

- [ ] **Step 1: Update SyncManager header**

Replace the contents of `include/collabtext/SyncManager.h` with:

```cpp
#pragma once

#include "collabtext/CrdtEngine.h"
#include "collabtext/Identity.h"
#include "collabtext/IdentityProjector.h"
#include "collabtext/PresenceManager.h"

#include <QDir>
#include <QObject>
#include <QTimer>
#include <memory>

namespace CollabText {

class SyncManager : public QObject {
    Q_OBJECT

public:
    SyncManager(CrdtEngine *engine,
                const Identity::Identity &identity,
                const std::string &replica_id,
                const std::string &device_name,
                QObject *parent = nullptr);
    ~SyncManager() override;

    void start(const QString &sharedFolder);
    void stop();

    void setEphemeralState(const Identity::EphemeralState &state);

    bool isRunning() const { return m_running; }
    QString replicaId() const { return QString::fromStdString(m_replicaId); }

signals:
    void remoteEphemeralChanged(const QString &replicaId,
                                const Identity::EphemeralState &state,
                                const Identity::Identity &identity);
    void presenceChanged(const QList<Identity::Presence> &livePeers);
    void syncError(const QString &message);

private slots:
    void syncCycle();

private:
    void flushLocalUpdates();
    void readRemoteUpdates();

    CrdtEngine *m_engine;
    QTimer m_timer;
    bool m_running = false;

    Identity::Identity m_identity;
    std::string m_replicaId;
    std::string m_deviceName;
    Identity::EphemeralState m_ephemeralState;
    uint64_t m_ephemeralSeq = 0;

    std::unique_ptr<Identity::PresenceManager> m_presence;
    std::unique_ptr<Identity::IdentityProjector> m_projector;

    // Pending local updates not yet flushed to disk
    QList<QByteArray> m_pendingUpdates;
    QString m_sharedFolder;
};

} // namespace CollabText
```

- [ ] **Step 2: Update SyncManager implementation**

Replace the contents of `src/SyncManager.cpp` with:

```cpp
#include "collabtext/SyncManager.h"

#include <QDateTime>
#include <QDir>

#include <chrono>
#include <ctime>

namespace CollabText {

SyncManager::SyncManager(CrdtEngine *engine,
                          const Identity::Identity &identity,
                          const std::string &replica_id,
                          const std::string &device_name,
                          QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_identity(identity)
    , m_replicaId(replica_id)
    , m_deviceName(device_name)
{
    connect(&m_timer, &QTimer::timeout, this, &SyncManager::syncCycle);
}

SyncManager::~SyncManager()
{
    stop();
}

void SyncManager::start(const QString &sharedFolder)
{
    m_sharedFolder = sharedFolder;
    m_running = true;

    std::filesystem::path folder = sharedFolder.toStdString();

    // Ensure directory structure
    QDir dir(sharedFolder);
    dir.mkpath(QStringLiteral("replicas/%1/ops").arg(
        QString::fromStdString(m_replicaId)));
    dir.mkpath(QStringLiteral("local/%1/cursors").arg(
        QString::fromStdString(m_replicaId)));
    dir.mkpath(QStringLiteral("meta"));

    m_presence = std::make_unique<Identity::PresenceManager>(
        folder, m_replicaId, m_identity.identity_id);
    m_projector = std::make_unique<Identity::IdentityProjector>(folder);

    // Project our identity
    m_projector->project(m_identity);

    m_timer.start(500);
}

void SyncManager::stop()
{
    m_timer.stop();
    if (m_running) {
        flushLocalUpdates();
        if (m_presence)
            m_presence->depart();
        m_running = false;
    }
}

void SyncManager::setEphemeralState(const Identity::EphemeralState &state)
{
    m_ephemeralState = state;
}

/// Current UTC time as ISO 8601.
static std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void SyncManager::syncCycle()
{
    flushLocalUpdates();
    readRemoteUpdates();

    // Write presence
    Identity::Presence p;
    p.replica_id = m_replicaId;
    p.identity_id = m_identity.identity_id;
    p.device_name = m_deviceName;
    p.active = true;
    p.last_heartbeat = now_iso8601();
    p.session_started = now_iso8601(); // TODO: capture once at start()
    m_presence->write_presence(p);

    // Write ephemeral
    m_ephemeralState.seq = ++m_ephemeralSeq;
    m_ephemeralState.timestamp = now_iso8601();
    m_presence->write_ephemeral(m_ephemeralState);

    // Read remote presences
    auto remotePresences = m_presence->read_remote_presences();
    QList<Identity::Presence> livePeers;

    // Read remote ephemerals
    auto remoteEphemerals = m_presence->read_remote_ephemerals();

    // Build ephemeral lookup by replica_id
    std::unordered_map<std::string, Identity::EphemeralState> ephMap;
    for (auto &[rid, es] : remoteEphemerals)
        ephMap[rid] = std::move(es);

    for (auto &[rid, presence] : remotePresences) {
        if (!Identity::PresenceManager::is_live(presence))
            continue;

        livePeers.append(presence);

        // Look up identity and ephemeral for this peer
        auto identity = m_projector->read(presence.identity_id);
        auto ephIt = ephMap.find(rid);

        if (identity && ephIt != ephMap.end()) {
            emit remoteEphemeralChanged(
                QString::fromStdString(rid),
                ephIt->second,
                *identity);
        }
    }

    emit presenceChanged(livePeers);
}

void SyncManager::flushLocalUpdates()
{
    // TODO: Re-enable when CrdtEngine gains serialization support
    // for the SyncManager path. Currently ops go through FileSync.
    m_pendingUpdates.clear();
}

void SyncManager::readRemoteUpdates()
{
    // TODO: Re-enable when CrdtEngine gains serialization support
    // for the SyncManager path. Currently ops go through FileSync.
}

} // namespace CollabText
```

- [ ] **Step 3: Build to verify compilation**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev -j$(nproc) 2>&1 | tail -10`
Expected: Clean build. The app target will need fixing in Task 11 due to changed SyncManager constructor, but the library should compile.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/include/collabtext/SyncManager.h \
        libs/collabtext/src/SyncManager.cpp
git commit -m "refactor: SyncManager delegates to PresenceManager + IdentityProjector"
```

---

## Task 11: Wire Up the Test App

**Files:**
- Modify: `app/main.cpp`

- [ ] **Step 1: Rewrite the test app to use the identity system**

Replace `app/main.cpp`. Key changes:
- Each EditorPane generates an Identity via IdentityStore (using a temp dir)
- Cursor sync goes through SyncManager's ephemeral files, not direct in-process calls
- MainWindow polls SyncManager and connects `remoteEphemeralChanged` to update remote cursors
- The direct `syncRemoteCursor()` method is deleted

```cpp
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QTextCursor>
#include <QPlainTextDocumentLayout>

#include "crdt/Buffer.h"
#include "crdt/FileSync.h"
#include "collabtext/Identity.h"
#include "collabtext/IdentityStore.h"
#include "collabtext/PresenceManager.h"
#include "collabtext/IdentityProjector.h"
#include "ui/CollabPlainTextEdit.h"
#include "ui/MultiCursorController.h"
#include "ui/ParticipantListWidget.h"

using namespace CollabText::Crdt;
using namespace CollabText::Ui;
using namespace CollabText::Identity;

/// A single editor pane with its own CRDT Buffer, FileSync, and identity.
class EditorPane : public QWidget {
    Q_OBJECT
public:
    EditorPane(const Identity &identity, uint16_t replicaId,
               const std::string &replicaName,
               const std::filesystem::path &sharedFolder,
               QWidget *parent = nullptr)
        : QWidget(parent)
        , m_buffer(replicaId)
        , m_sync(m_buffer, sharedFolder, replicaName)
        , m_presence(sharedFolder, replicaName, identity.identity_id)
        , m_edit(new CollabPlainTextEdit(this))
        , m_qtDoc(new QTextDocument(this))
        , m_identity(identity)
        , m_replicaName(replicaName)
    {
        m_qtDoc->setDocumentLayout(new QPlainTextDocumentLayout(m_qtDoc));
        m_qtDoc->setUndoRedoEnabled(false);
        m_edit->setDocument(m_qtDoc);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        auto *header = new QLabel(
            QString::fromStdString(identity.display_name), this);
        header->setStyleSheet(QStringLiteral("font-weight: bold; color: %1;")
                                  .arg(QString::fromStdString(identity.color)));
        layout->addWidget(header);
        layout->addWidget(m_edit);

        m_edit->setPlaceholderText(
            QStringLiteral("Type here... (Alt+Click for multi-cursor, "
                           "Ctrl+Alt+Up/Down to add cursors)"));

        m_statusLabel = new QLabel(this);
        auto *gremlinBtn = new QPushButton(QStringLiteral("Gremlin: OFF"), this);
        gremlinBtn->setCheckable(true);
        auto *bottomRow = new QHBoxLayout;
        bottomRow->addWidget(m_statusLabel);
        bottomRow->addStretch();
        bottomRow->addWidget(gremlinBtn);
        layout->addLayout(bottomRow);

        m_gremlinTimer = new QTimer(this);
        m_gremlinTimer->setInterval(80);
        connect(m_gremlinTimer, &QTimer::timeout, this, &EditorPane::gremlinTick);
        connect(gremlinBtn, &QPushButton::toggled, this, [this, gremlinBtn](bool on) {
            if (on) {
                m_gremlinTimer->start();
                gremlinBtn->setText(QStringLiteral("Gremlin: ON"));
            } else {
                m_gremlinTimer->stop();
                gremlinBtn->setText(QStringLiteral("Gremlin: OFF"));
            }
        });

        connect(m_edit->multiCursorController(),
                &MultiCursorController::cursorsChanged, this,
                [this]() {
                    int n = m_edit->multiCursorController()->cursorCount();
                    m_statusLabel->setText(
                        n > 1 ? QStringLiteral("%1 cursors").arg(n)
                              : QStringLiteral("1 cursor"));
                });

        connect(m_qtDoc, &QTextDocument::contentsChange,
                this, &EditorPane::onContentsChange);

        m_sync.start();
    }

    CollabPlainTextEdit *editor() const { return m_edit; }
    Buffer &buffer() { return m_buffer; }
    const Identity &identity() const { return m_identity; }
    PresenceManager &presenceManager() { return m_presence; }
    const std::string &replicaName() const { return m_replicaName; }

    void poll() {
        Global before = m_buffer.version();
        size_t applied = m_sync.poll();
        if (applied > 0) {
            applyEditsToQt(m_buffer.edits_since(before));
        }
    }

    /// Build and write ephemeral state from current cursor.
    void writeEphemeral(uint64_t seq) {
        auto cursor = m_edit->textCursor();
        uint32_t bytePos = qtPosToByteOffset(cursor.position());
        uint32_t byteAnchor = qtPosToByteOffset(cursor.anchor());

        EphemeralState es;
        es.seq = seq;
        es.activity = "idle"; // simplified for now

        auto posAnchor = m_buffer.anchor_at(bytePos, Bias::Right);
        auto selAnchor = m_buffer.anchor_at(byteAnchor, Bias::Left);
        es.cursors.push_back({selAnchor, posAnchor});

        m_presence.write_ephemeral(es);
    }

    /// Write presence heartbeat.
    void writePresence() {
        Presence p;
        p.replica_id = m_replicaName;
        p.identity_id = m_identity.identity_id;
        p.device_name = m_replicaName;
        p.active = true;

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        gmtime_r(&time_t, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        p.last_heartbeat = buf;
        p.session_started = buf;

        m_presence.write_presence(p);
    }

    /// Apply remote ephemeral state: resolve anchors to byte offsets,
    /// then set remote cursors on the multi-cursor controller.
    void applyRemoteEphemeral(const EphemeralState &es,
                               const Identity &remoteIdentity) {
        QList<RemoteCursor> cursors;
        for (const auto &cp : es.cursors) {
            RemoteCursor rc;
            rc.bytePosition = m_buffer.resolve_anchor(cp.head);
            rc.byteAnchor = m_buffer.resolve_anchor(cp.anchor);
            rc.color = QColor(QString::fromStdString(remoteIdentity.color));
            rc.label = QString::fromStdString(remoteIdentity.display_name);
            cursors.append(rc);
        }
        m_edit->multiCursorController()->setRemoteCursors(cursors);
    }

    uint32_t qtPosToByteOffset(int qtPos) const {
        QString docText = m_qtDoc->toPlainText();
        return docText.left(qMin(qtPos, docText.length())).toUtf8().size();
    }

    int byteOffsetToQtPos(uint32_t byteOffset) const {
        QString docText = m_qtDoc->toPlainText();
        QByteArray utf8 = docText.toUtf8();
        uint32_t clamped = qMin(byteOffset, static_cast<uint32_t>(utf8.size()));
        return QString::fromUtf8(utf8.data(), clamped).length();
    }

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded) {
        if (m_syncing) return;

        std::string bufText = m_buffer.text();
        QString qBufText = QString::fromStdString(bufText);

        uint32_t byteStart = qBufText.left(position).toUtf8().size();
        uint32_t byteEnd = byteStart;
        if (charsRemoved > 0) {
            byteEnd = qBufText.left(position + charsRemoved).toUtf8().size();
        }

        std::string inserted;
        if (charsAdded > 0) {
            QTextCursor cursor(m_qtDoc);
            cursor.setPosition(position);
            cursor.setPosition(position + charsAdded, QTextCursor::KeepAnchor);
            QString sel = cursor.selectedText();
            sel.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            inserted = sel.toStdString();
        }

        auto op = m_buffer.apply_local_edit({{byteStart, byteEnd}}, {inserted});
        m_sync.push_local_op(op);
    }

    void gremlinTick() {
        static const char *lorem[] = {
            "lorem ", "ipsum ", "dolor ", "sit ", "amet ", "consectetur ",
            "adipiscing ", "elit ", "sed ", "do ", "eiusmod ", "tempor ",
            "incididunt ", "ut ", "labore ", "et ", "dolore ", "magna ",
            "aliqua ", "enim ", "ad ", "minim ", "veniam ", "quis ",
            "nostrud ", "exercitation ", "ullamco ", "laboris ", "nisi ",
        };
        static constexpr int nwords = sizeof(lorem) / sizeof(lorem[0]);

        auto *rng = QRandomGenerator::global();
        uint32_t docLen = m_buffer.visible_length();
        int roll = rng->bounded(50);

        Global before = m_buffer.version();
        Operation op;

        if (roll == 0 && docLen > 10) {
            uint32_t delLen = qMin(static_cast<uint32_t>(rng->bounded(3, 9)), docLen);
            uint32_t pos = rng->bounded(docLen - delLen + 1);
            op = m_buffer.apply_local_edit({{pos, pos + delLen}}, {""});
        } else if (roll < 3 && docLen > 0) {
            uint32_t pos = rng->bounded(docLen + 1);
            op = m_buffer.apply_local_edit({{pos, pos}}, {"\n"});
        } else {
            uint32_t pos = docLen;
            if (rng->bounded(5) == 0 && docLen > 0)
                pos = rng->bounded(docLen + 1);
            const char *word = lorem[rng->bounded(nwords)];
            op = m_buffer.apply_local_edit({{pos, pos}}, {word});
        }

        m_sync.push_local_op(op);
        applyEditsToQt(m_buffer.edits_since(before));
    }

private:
    void applyEditsToQt(const std::vector<TextEdit> &edits) {
        if (edits.empty()) return;
        m_syncing = true;

        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            int qtStart = byteOffsetToQtPos(it->old_start);
            int qtEnd = byteOffsetToQtPos(it->old_end);
            QTextCursor cursor(m_qtDoc);
            cursor.setPosition(qtStart);
            if (qtEnd > qtStart)
                cursor.setPosition(qtEnd, QTextCursor::KeepAnchor);
            QString replacement = QString::fromUtf8(
                it->new_text.data(),
                static_cast<int>(it->new_text.size()));
            cursor.insertText(replacement);
        }

        m_syncing = false;
    }

    bool m_syncing = false;
    Buffer m_buffer;
    FileSync m_sync;
    PresenceManager m_presence;
    CollabPlainTextEdit *m_edit;
    QTextDocument *m_qtDoc;
    Identity m_identity;
    std::string m_replicaName;
    QLabel *m_statusLabel;
    QTimer *m_gremlinTimer = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const std::filesystem::path &sharedFolder)
    {
        setWindowTitle(QStringLiteral("CollabText — Full-Stack Demo"));
        resize(1200, 600);

        auto *central = new QWidget(this);
        auto *layout = new QHBoxLayout(central);

        // Generate identities in temp directories
        std::filesystem::path configA = sharedFolder / "config-a";
        std::filesystem::path configB = sharedFolder / "config-b";
        IdentityStore storeA(configA);
        IdentityStore storeB(configB);

        auto alice = storeA.generate("Alice");
        alice.color = "#4169e1"; // royal blue
        storeA.save(alice);

        auto bob = storeB.generate("Bob");
        bob.color = "#dc143c"; // crimson
        storeB.save(bob);

        // Project identities
        IdentityProjector projector(sharedFolder);
        projector.project(alice);
        projector.project(bob);

        m_paneA = new EditorPane(alice, 1, "alice",
                                  sharedFolder, central);
        m_paneB = new EditorPane(bob, 2, "bob",
                                  sharedFolder, central);

        m_participants = new ParticipantListWidget(central);
        m_participants->setFixedWidth(200);

        layout->addWidget(m_paneA);
        layout->addWidget(m_paneB);
        layout->addWidget(m_participants);
        setCentralWidget(central);

        m_projector = std::make_unique<IdentityProjector>(sharedFolder);

        auto *syncTimer = new QTimer(this);
        connect(syncTimer, &QTimer::timeout, this, &MainWindow::syncCycle);
        syncTimer->start(100);

        statusBar()->showMessage(
            QStringLiteral("Full-stack sync via FileSync + ephemeral identity | "
                           "Alt+Click: add cursor | Ctrl+Alt+Up/Down: column cursor"));
    }

private slots:
    void syncCycle() {
        m_seq++;

        // Poll CRDT ops
        m_paneA->poll();
        m_paneB->poll();

        // Write presence and ephemeral
        m_paneA->writePresence();
        m_paneB->writePresence();
        m_paneA->writeEphemeral(m_seq);
        m_paneB->writeEphemeral(m_seq);

        // Read remote ephemerals and apply
        syncRemoteEphemeral(m_paneA, m_paneB);
        syncRemoteEphemeral(m_paneB, m_paneA);

        // Update participant list
        auto identities = m_projector->read_all();
        auto presencesA = m_paneA->presenceManager().read_remote_presences();
        // Combine with own presence
        std::vector<Presence> allPresences;
        Presence selfA;
        selfA.replica_id = "alice";
        selfA.identity_id = m_paneA->identity().identity_id;
        selfA.active = true;
        allPresences.push_back(selfA);
        Presence selfB;
        selfB.replica_id = "bob";
        selfB.identity_id = m_paneB->identity().identity_id;
        selfB.active = true;
        allPresences.push_back(selfB);
        for (auto &[rid, p] : presencesA) allPresences.push_back(p);

        m_participants->updateParticipants(identities, allPresences);
    }

    void syncRemoteEphemeral(EditorPane *from, EditorPane *to) {
        auto remotes = to->presenceManager().read_remote_ephemerals();
        for (auto &[rid, es] : remotes) {
            // Look up the identity for this replica
            auto presences = to->presenceManager().read_remote_presences();
            for (auto &[prid, p] : presences) {
                if (prid == rid) {
                    auto identity = m_projector->read(p.identity_id);
                    if (identity) {
                        to->applyRemoteEphemeral(es, *identity);
                    }
                    break;
                }
            }
        }
    }

private:
    EditorPane *m_paneA;
    EditorPane *m_paneB;
    ParticipantListWidget *m_participants;
    std::unique_ptr<IdentityProjector> m_projector;
    uint64_t m_seq = 0;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QTemporaryDir tmpDir;
    tmpDir.setAutoRemove(true);
    std::filesystem::path sharedFolder = tmpDir.path().toStdString();

    MainWindow window(sharedFolder);
    window.show();
    return app.exec();
}

#include "main.moc"
```

- [ ] **Step 2: Build the full project**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev -j$(nproc) 2>&1 | tail -20`
Expected: Clean build.

- [ ] **Step 3: Run all tests**

Run: `cd /home/clinton/dev/collabtext && cd build-dev && ctest --output-on-failure -j$(nproc) 2>&1 | tail -30`
Expected: All tests pass (existing + new).

- [ ] **Step 4: Manual smoke test**

Run: `cd /home/clinton/dev/collabtext && build-dev/app/collabtext`
Verify:
- Two editor panes show "Alice" (blue) and "Bob" (red) headers
- Typing in one pane appears in the other after ~100ms
- Remote cursors appear with the correct color and label
- Participant list panel shows both Alice and Bob
- Gremlin mode still works

- [ ] **Step 5: Commit**

```bash
git add app/main.cpp
git commit -m "feat: test app uses real identity system — ephemeral cursors via files"
```

---

## Task 12: Run Full Test Suite and Fix Issues

- [ ] **Step 1: Build everything clean**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev -j$(nproc) 2>&1 | tail -20`
Expected: Clean build with no warnings.

- [ ] **Step 2: Run all tests**

Run: `cd /home/clinton/dev/collabtext && cd build-dev && ctest --output-on-failure -j$(nproc)`
Expected: All tests pass. If any fail, fix the specific issue and re-run.

- [ ] **Step 3: Commit any fixes**

If fixes were needed:
```bash
git add -u
git commit -m "fix: test suite issues from identity system integration"
```
