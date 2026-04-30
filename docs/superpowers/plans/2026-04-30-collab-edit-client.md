# Collab-Edit Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone single-window Qt6 client (`collabedit`) that two users on different machines can run against a Syncthing-mirrored folder to validate the CRDT engine end-to-end. Per-document opt-in collab via persistent sidecar; deterministic seeding for safe concurrent enrollment.

**Architecture:** New executable under `app/collabedit/`, built alongside the existing `collabtext-testapp`. Two new library modules: `SeedOp` (deterministic CRDT seed from text) and `SidecarManifest` (sidecar metadata). The app has a `Document` state machine with two modes: Plain (`QPlainTextEdit` over a normal file) and Collab (`Buffer` + `FileSync` + `PresenceManager` over a sidecar). Reuses existing `IdentityStore`, `IdentitySetupDialog`, `CollabPlainTextEdit`, `MultiCursorController`, `ParticipantListWidget`.

**Tech Stack:** Qt 6.8 (Core, Gui, Widgets, Test), C++20, CMake ≥3.19, existing CollabText library. Build dir: `build-dev/` (preset project).

**Spec:** `docs/superpowers/specs/2026-04-30-collab-edit-client-design.md`

---

## File Structure

**New files (library):**
- `libs/collabtext/src/crdt/SeedOp.h` — declaration of `op_for_seed(content)`
- `libs/collabtext/src/crdt/SeedOp.cpp` — implementation
- `libs/collabtext/src/crdt/SidecarManifest.h` — manifest schema, read/write/compare
- `libs/collabtext/src/crdt/SidecarManifest.cpp` — implementation
- `libs/collabtext/tests/tst_seed_op.cpp` — determinism + cross-replica tests
- `libs/collabtext/tests/tst_sidecar_manifest.cpp` — schema, sha mismatch, doc_id compare

**New files (app):**
- `app/CMakeLists.txt` — new (currently `app/main.cpp` is built directly via `add_subdirectory(app)` and `app/CMakeLists.txt`); we reorganize so `app/CMakeLists.txt` adds two subdirs.
- `app/testapp/CMakeLists.txt` — moved from `app/CMakeLists.txt`
- `app/testapp/main.cpp` — moved from `app/main.cpp`
- `app/collabedit/CMakeLists.txt` — build target `collabedit`
- `app/collabedit/main.cpp` — `QApplication`, identity bootstrap, CLI arg
- `app/collabedit/MainWindow.h/.cpp` — top-level window, menus, status bar
- `app/collabedit/Document.h/.cpp` — Plain/Collab state machine
- `app/collabedit/CollabPane.h/.cpp` — Collab-mode editor, owns Buffer/FileSync/Presence

**Modified files:**
- `libs/collabtext/CMakeLists.txt` — register new sources + tests
- `app/CMakeLists.txt` — replaced by reorganized version

---

## Task 1: SeedOp — failing test for deterministic seed

**Files:**
- Create: `libs/collabtext/tests/tst_seed_op.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `libs/collabtext/tests/tst_seed_op.cpp`:

```cpp
#include <QTest>
#include "crdt/SeedOp.h"
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestSeedOp : public QObject {
    Q_OBJECT
private slots:
    void empty_seed_produces_empty_buffer() {
        Buffer buf(7);
        buf.apply_ops({op_for_seed("")});
        QCOMPARE(buf.text(), std::string(""));
    }

    void seed_text_visible_after_apply() {
        Buffer buf(42);
        buf.apply_ops({op_for_seed("hello world")});
        QCOMPARE(buf.text(), std::string("hello world"));
    }

    void seed_is_deterministic_across_replicas() {
        // Two buffers with different replica ids should converge to the
        // same visible text after applying the same seed op.
        Buffer a(1);
        Buffer b(2);
        const std::string seed = "Line one\nLine two\n";
        auto op_a = op_for_seed(seed);
        auto op_b = op_for_seed(seed);
        a.apply_ops({op_a});
        b.apply_ops({op_b});
        QCOMPARE(a.text(), seed);
        QCOMPARE(b.text(), seed);
    }

    void seed_op_uses_replica_zero() {
        // The seed op's timestamp must use the synthetic replica id 0,
        // so the op identity is independent of the joining replica.
        auto op = op_for_seed("anything");
        const auto* edit = std::get_if<EditOperation>(&op);
        QVERIFY(edit != nullptr);
        QCOMPARE(edit->timestamp.replica_id, uint16_t(0));
    }

    void edits_compose_with_seed() {
        // Apply seed then a normal local edit; verify both are present.
        Buffer buf(5);
        buf.apply_ops({op_for_seed("abc")});
        buf.apply_local_edit({{3, 3}}, {"def"});
        QCOMPARE(buf.text(), std::string("abcdef"));
    }
};

QTEST_GUILESS_MAIN(TestSeedOp)
#include "tst_seed_op.moc"
```

- [ ] **Step 2: Register the test in CMake**

Append to `libs/collabtext/CMakeLists.txt` after the last `add_crdt_test` line:

```cmake
add_crdt_test(tst_seed_op)
```

- [ ] **Step 3: Run test to verify it fails to compile (header missing)**

Run: `cmake --build build-dev --target tst_seed_op 2>&1 | tail -20`
Expected: compile error mentioning `crdt/SeedOp.h: No such file`.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/tests/tst_seed_op.cpp libs/collabtext/CMakeLists.txt
git commit -m "test(seed): failing tests for deterministic CRDT seed op"
```

---

## Task 2: SeedOp — implementation

**Files:**
- Create: `libs/collabtext/src/crdt/SeedOp.h`
- Create: `libs/collabtext/src/crdt/SeedOp.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `libs/collabtext/src/crdt/SeedOp.h`:

```cpp
#pragma once

#include "crdt/Operations.h"

#include <string>

namespace CollabText::Crdt {

/// Build a deterministic CRDT operation that inserts the given seed
/// content into an empty buffer. The op uses a synthetic seed replica
/// (replica_id = 0) so two peers computing it from the same input
/// produce byte-identical operations — applying them on either side
/// converges to the same buffer state.
///
/// Usage on join:
///   Buffer buf(my_replica_id);            // real replica id
///   buf.apply_ops({op_for_seed(seed)});   // seed CRDT state
///   // ... continue with normal sync ...
///
/// The seed op is never broadcast via FileSync; each replica
/// reconstructs it locally from `seed.txt`.
Operation op_for_seed(const std::string& content);

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Write the implementation**

Create `libs/collabtext/src/crdt/SeedOp.cpp`:

```cpp
#include "crdt/SeedOp.h"
#include "crdt/Buffer.h"

namespace CollabText::Crdt {

Operation op_for_seed(const std::string& content) {
    // Construct the op from a fresh Buffer with the synthetic seed
    // replica id. apply_local_edit produces an EditOperation whose
    // timestamp, locators, and inserted-fragment ids depend only on
    // the buffer's replica id, its (empty) prior state, and the
    // content — all of which are identical across replicas.
    Buffer seed_buffer(0);
    return seed_buffer.apply_local_edit({{0, 0}}, {content});
}

} // namespace CollabText::Crdt
```

- [ ] **Step 3: Register source in CMake**

Edit `libs/collabtext/CMakeLists.txt`. In the `add_library(collabtext STATIC ...)` source list, add `src/crdt/SeedOp.cpp` near the other `src/crdt/*.cpp` entries (alphabetical-ish order alongside `Serialization.cpp`):

```cmake
    src/crdt/SeedOp.cpp
```

- [ ] **Step 4: Build and run the test**

Run: `cmake --build build-dev --target tst_seed_op && ctest --test-dir build-dev -R '^tst_seed_op$' --output-on-failure`
Expected: PASS, all 5 cases.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/crdt/SeedOp.h libs/collabtext/src/crdt/SeedOp.cpp libs/collabtext/CMakeLists.txt
git commit -m "feat(seed): deterministic CRDT seed op via reserved replica 0"
```

---

## Task 3: SidecarManifest — failing test

**Files:**
- Create: `libs/collabtext/tests/tst_sidecar_manifest.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `libs/collabtext/tests/tst_sidecar_manifest.cpp`:

```cpp
#include <QTest>
#include <QTemporaryDir>
#include "crdt/SidecarManifest.h"

#include <filesystem>
#include <fstream>

using namespace CollabText::Crdt;
namespace fs = std::filesystem;

class TestSidecarManifest : public QObject {
    Q_OBJECT
private slots:
    void round_trip() {
        SidecarManifest m;
        m.schema_version    = 1;
        m.doc_id            = "01HXXX0000000000000000000A";
        m.enrolled_at       = "2026-04-30T14:22:01Z";
        m.original_filename = "notes.md";
        m.seed_sha256       = "deadbeef";

        std::string json = manifest_to_json(m);
        auto parsed = manifest_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->schema_version,    m.schema_version);
        QCOMPARE(parsed->doc_id,            m.doc_id);
        QCOMPARE(parsed->enrolled_at,       m.enrolled_at);
        QCOMPARE(parsed->original_filename, m.original_filename);
        QCOMPARE(parsed->seed_sha256,       m.seed_sha256);
    }

    void rejects_unknown_schema_version() {
        std::string json = R"({"schema_version":99,"doc_id":"x","enrolled_at":"t","original_filename":"f","seed_sha256":"h"})";
        QVERIFY(!manifest_from_json(json).has_value());
    }

    void rejects_malformed_json() {
        QVERIFY(!manifest_from_json("").has_value());
        QVERIFY(!manifest_from_json("{").has_value());
        QVERIFY(!manifest_from_json("not json").has_value());
    }

    void sha256_helper_matches_known_value() {
        // SHA-256 of "abc" is the canonical test vector.
        std::string hex = sha256_hex("abc");
        QCOMPARE(hex, std::string(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }

    void sha256_helper_empty_input() {
        // Standard empty-string SHA-256.
        QCOMPARE(sha256_hex(""), std::string(
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    }

    void atomic_write_and_read_round_trip() {
        QTemporaryDir tmp;
        fs::path dir = tmp.path().toStdString();

        SidecarManifest m;
        m.schema_version = 1;
        m.doc_id         = "01HABC";
        m.enrolled_at    = "2026-04-30T14:22:01Z";
        m.original_filename = "notes.md";
        m.seed_sha256    = sha256_hex("hello");

        auto path = dir / "manifest.json";
        write_manifest(path, m);
        QVERIFY(fs::exists(path));

        auto loaded = read_manifest(path);
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->doc_id, m.doc_id);
    }

    void doc_id_compare_is_lexicographic() {
        QVERIFY(doc_id_less("01HABC", "01HABD"));
        QVERIFY(!doc_id_less("01HABD", "01HABC"));
        QVERIFY(!doc_id_less("01HABC", "01HABC"));
    }
};

QTEST_GUILESS_MAIN(TestSidecarManifest)
#include "tst_sidecar_manifest.moc"
```

- [ ] **Step 2: Register the test in CMake**

Append to `libs/collabtext/CMakeLists.txt`:

```cmake
add_crdt_test(tst_sidecar_manifest)
```

- [ ] **Step 3: Verify it fails to compile**

Run: `cmake --build build-dev --target tst_sidecar_manifest 2>&1 | tail -10`
Expected: missing-header error.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/tests/tst_sidecar_manifest.cpp libs/collabtext/CMakeLists.txt
git commit -m "test(manifest): failing tests for sidecar manifest schema"
```

---

## Task 4: SidecarManifest — implementation

**Files:**
- Create: `libs/collabtext/src/crdt/SidecarManifest.h`
- Create: `libs/collabtext/src/crdt/SidecarManifest.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `libs/collabtext/src/crdt/SidecarManifest.h`:

```cpp
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace CollabText::Crdt {

struct SidecarManifest {
    int         schema_version = 1;
    std::string doc_id;
    std::string enrolled_at;        // ISO 8601 UTC
    std::string original_filename;
    std::string seed_sha256;        // hex
};

/// SHA-256 of arbitrary bytes, lower-case hex.
std::string sha256_hex(const std::string& data);

/// Serialize a manifest to a one-line JSON string.
std::string manifest_to_json(const SidecarManifest& m);

/// Parse a manifest. Returns nullopt for malformed JSON or
/// schema_version != 1.
std::optional<SidecarManifest> manifest_from_json(const std::string& json);

/// Atomically write the manifest to a path (write-temp + rename).
void write_manifest(const std::filesystem::path& path,
                    const SidecarManifest& m);

/// Read and parse a manifest from a path. Returns nullopt on any
/// failure (missing, malformed, schema mismatch).
std::optional<SidecarManifest> read_manifest(const std::filesystem::path& path);

/// Compare two doc_ids by lexicographic string order. Smaller wins
/// the enrollment-race tiebreaker.
bool doc_id_less(const std::string& a, const std::string& b);

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Write the implementation**

Create `libs/collabtext/src/crdt/SidecarManifest.cpp`:

```cpp
#include "crdt/SidecarManifest.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

std::string sha256_hex(const std::string& data) {
    QByteArray digest = QCryptographicHash::hash(
        QByteArray::fromStdString(data),
        QCryptographicHash::Sha256);
    return digest.toHex().toStdString();
}

bool doc_id_less(const std::string& a, const std::string& b) {
    return a < b;
}

// ---- minimal JSON helpers (consistent with Comment.cpp style) ----

static std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
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

std::string manifest_to_json(const SidecarManifest& m) {
    std::ostringstream o;
    o << "{"
      << "\"schema_version\":" << m.schema_version
      << ",\"doc_id\":" << escape(m.doc_id)
      << ",\"enrolled_at\":" << escape(m.enrolled_at)
      << ",\"original_filename\":" << escape(m.original_filename)
      << ",\"seed_sha256\":" << escape(m.seed_sha256)
      << "}";
    return o.str();
}

namespace {

struct Parser {
    std::string_view src;
    size_t pos = 0;

    void skip_ws() {
        while (pos < src.size()
               && (src[pos] == ' ' || src[pos] == '\t'
                   || src[pos] == '\n' || src[pos] == '\r'))
            ++pos;
    }

    bool eat(char c) {
        skip_ws();
        if (pos < src.size() && src[pos] == c) { ++pos; return true; }
        return false;
    }

    std::optional<std::string> parse_string() {
        skip_ws();
        if (pos >= src.size() || src[pos] != '"') return std::nullopt;
        ++pos;
        std::string out;
        while (pos < src.size() && src[pos] != '"') {
            char c = src[pos++];
            if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                switch (esc) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'u': {
                        if (pos + 4 > src.size()) return std::nullopt;
                        unsigned int u = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src[pos++];
                            u <<= 4;
                            if (h >= '0' && h <= '9') u |= (h - '0');
                            else if (h >= 'a' && h <= 'f') u |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') u |= (h - 'A' + 10);
                            else return std::nullopt;
                        }
                        if (u < 0x80) {
                            out += static_cast<char>(u);
                        } else {
                            // Limited handling: ASCII-range only.
                            // Manifests are ASCII in practice.
                            out += '?';
                        }
                        break;
                    }
                    default: return std::nullopt;
                }
            } else {
                out += c;
            }
        }
        if (pos >= src.size()) return std::nullopt;
        ++pos; // closing quote
        return out;
    }

    std::optional<int> parse_int() {
        skip_ws();
        size_t start = pos;
        if (pos < src.size() && (src[pos] == '-' || src[pos] == '+')) ++pos;
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
        if (start == pos) return std::nullopt;
        try {
            return std::stoi(std::string(src.substr(start, pos - start)));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<std::string> parse_key() {
        return parse_string();
    }
};

} // namespace

std::optional<SidecarManifest> manifest_from_json(const std::string& json) {
    Parser p{json};
    if (!p.eat('{')) return std::nullopt;

    SidecarManifest m;
    m.schema_version = -1;
    bool first = true;

    while (true) {
        p.skip_ws();
        if (p.pos < p.src.size() && p.src[p.pos] == '}') { ++p.pos; break; }
        if (!first && !p.eat(',')) return std::nullopt;
        first = false;

        auto key = p.parse_key();
        if (!key) return std::nullopt;
        if (!p.eat(':')) return std::nullopt;

        if (*key == "schema_version") {
            auto v = p.parse_int();
            if (!v) return std::nullopt;
            m.schema_version = *v;
        } else if (*key == "doc_id") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            m.doc_id = *v;
        } else if (*key == "enrolled_at") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            m.enrolled_at = *v;
        } else if (*key == "original_filename") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            m.original_filename = *v;
        } else if (*key == "seed_sha256") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            m.seed_sha256 = *v;
        } else {
            // Unknown key: skip its string value (best-effort).
            auto _ = p.parse_string();
            if (!_) return std::nullopt;
        }
    }

    if (m.schema_version != 1) return std::nullopt;
    return m;
}

void write_manifest(const fs::path& path, const SidecarManifest& m) {
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("write_manifest: open tmp failed: " + tmp.string());
        std::string json = manifest_to_json(m);
        f.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!f) throw std::runtime_error("write_manifest: write failed: " + tmp.string());
    }
    fs::rename(tmp, path);
}

std::optional<SidecarManifest> read_manifest(const fs::path& path) {
    if (!fs::exists(path)) return std::nullopt;
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    return manifest_from_json(content);
}

} // namespace CollabText::Crdt
```

- [ ] **Step 3: Register source in CMake**

Edit `libs/collabtext/CMakeLists.txt`. Add to the `add_library(collabtext STATIC ...)` source list, alongside the other `src/crdt/*.cpp` entries:

```cmake
    src/crdt/SidecarManifest.cpp
```

- [ ] **Step 4: Build and run the test**

Run: `cmake --build build-dev --target tst_sidecar_manifest && ctest --test-dir build-dev -R '^tst_sidecar_manifest$' --output-on-failure`
Expected: PASS, all 6 cases.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/crdt/SidecarManifest.h libs/collabtext/src/crdt/SidecarManifest.cpp libs/collabtext/CMakeLists.txt
git commit -m "feat(manifest): SidecarManifest schema, JSON, atomic IO, sha256"
```

---

## Task 5: Reorganize app/ into testapp/ and collabedit/ subdirs

**Files:**
- Move: `app/main.cpp` → `app/testapp/main.cpp`
- Move: `app/CMakeLists.txt` → `app/testapp/CMakeLists.txt`
- Create: `app/CMakeLists.txt` (new aggregator)

- [ ] **Step 1: Move existing files into testapp/**

```bash
mkdir app/testapp
git mv app/main.cpp app/testapp/main.cpp
git mv app/CMakeLists.txt app/testapp/CMakeLists.txt
```

- [ ] **Step 2: Update the moved CMakeLists.txt include path**

Edit `app/testapp/CMakeLists.txt`. The include path line currently says `${CMAKE_CURRENT_SOURCE_DIR}/../libs/...`; with the new depth, this becomes `../../libs/...`:

```cmake
target_include_directories(collabtext-testapp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/collabtext/src)
```

- [ ] **Step 3: Create the aggregator app/CMakeLists.txt**

Create `app/CMakeLists.txt`:

```cmake
add_subdirectory(testapp)
add_subdirectory(collabedit)
```

- [ ] **Step 4: Create a stub app/collabedit/ to keep CMake configure working**

Create `app/collabedit/CMakeLists.txt` as a placeholder (will be filled in next task):

```cmake
# Placeholder — the collabedit target is added in the next task.
```

- [ ] **Step 5: Reconfigure and rebuild testapp to verify reorganization**

Run: `cmake --build build-dev --target collabtext-testapp 2>&1 | tail -10`
Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
git add app/CMakeLists.txt app/testapp/CMakeLists.txt app/testapp/main.cpp app/collabedit/CMakeLists.txt
# (the moves are already staged from `git mv`)
git commit -m "build: reorganize app/ into testapp/ + collabedit/ subdirs"
```

---

## Task 6: collabedit skeleton — main.cpp + MainWindow + identity bootstrap

**Files:**
- Create: `app/collabedit/main.cpp`
- Create: `app/collabedit/MainWindow.h`
- Create: `app/collabedit/MainWindow.cpp`
- Modify: `app/collabedit/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `app/collabedit/MainWindow.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"
#include "collabtext/IdentityStore.h"

#include <QMainWindow>

#include <memory>
#include <string>

class QAction;
class QLabel;
class QPushButton;
class QPlainTextEdit;

namespace CollabEdit {

class Document;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(CollabText::Identity::Identity identity,
               std::string replica_name,
               QWidget *parent = nullptr);
    ~MainWindow() override;

    /// Open the given file (Plain mode unless a sidecar exists, which
    /// causes Collab mode). An empty path means "no file open".
    void openFile(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onFileNew();
    void onFileOpen();
    void onFileSave();
    void onFileSaveAs();
    void onFileClose();
    void onEnableCollab();
    void onAbout();

private:
    void buildMenus();
    void buildStatusBar();
    void updateTitleAndStatus();

    CollabText::Identity::Identity m_identity;
    std::string m_replicaName;

    std::unique_ptr<Document> m_doc;

    QAction *m_actEnableCollab = nullptr;
    QAction *m_actSave = nullptr;
    QAction *m_actSaveAs = nullptr;
    QAction *m_actClose = nullptr;

    QLabel *m_statusLabel = nullptr;
    QPushButton *m_enableCollabBtn = nullptr;
};

} // namespace CollabEdit
```

- [ ] **Step 2: Write the implementation (skeleton — Document is a stub for now)**

Create `app/collabedit/MainWindow.cpp`:

```cpp
#include "MainWindow.h"
#include "Document.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace CollabEdit {

MainWindow::MainWindow(CollabText::Identity::Identity identity,
                       std::string replica_name,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_identity(std::move(identity))
    , m_replicaName(std::move(replica_name))
{
    m_doc = std::make_unique<Document>(m_identity, m_replicaName, this);
    setCentralWidget(m_doc->widget());
    connect(m_doc.get(), &Document::changed, this, &MainWindow::updateTitleAndStatus);

    buildMenus();
    buildStatusBar();
    resize(900, 700);
    updateTitleAndStatus();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    auto *actNew = fileMenu->addAction(tr("&New"));
    actNew->setShortcut(QKeySequence::New);
    connect(actNew, &QAction::triggered, this, &MainWindow::onFileNew);

    auto *actOpen = fileMenu->addAction(tr("&Open..."));
    actOpen->setShortcut(QKeySequence::Open);
    connect(actOpen, &QAction::triggered, this, &MainWindow::onFileOpen);

    fileMenu->addSeparator();

    m_actSave = fileMenu->addAction(tr("&Save"));
    m_actSave->setShortcut(QKeySequence::Save);
    connect(m_actSave, &QAction::triggered, this, &MainWindow::onFileSave);

    m_actSaveAs = fileMenu->addAction(tr("Save &As..."));
    m_actSaveAs->setShortcut(QKeySequence::SaveAs);
    connect(m_actSaveAs, &QAction::triggered, this, &MainWindow::onFileSaveAs);

    fileMenu->addSeparator();

    m_actClose = fileMenu->addAction(tr("&Close"));
    m_actClose->setShortcut(QKeySequence::Close);
    connect(m_actClose, &QAction::triggered, this, &MainWindow::onFileClose);

    auto *actQuit = fileMenu->addAction(tr("&Quit"));
    actQuit->setShortcut(QKeySequence::Quit);
    connect(actQuit, &QAction::triggered, qApp, &QApplication::closeAllWindows);

    auto *docMenu = menuBar()->addMenu(tr("&Document"));
    m_actEnableCollab = docMenu->addAction(tr("&Enable Collab"));
    connect(m_actEnableCollab, &QAction::triggered, this, &MainWindow::onEnableCollab);

    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    auto *actAbout = helpMenu->addAction(tr("&About"));
    connect(actAbout, &QAction::triggered, this, &MainWindow::onAbout);
}

void MainWindow::buildStatusBar() {
    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel, 1);

    m_enableCollabBtn = new QPushButton(tr("Enable Collab"), this);
    statusBar()->addPermanentWidget(m_enableCollabBtn);
    connect(m_enableCollabBtn, &QPushButton::clicked, this, &MainWindow::onEnableCollab);
}

void MainWindow::updateTitleAndStatus() {
    QString name = m_doc->displayName();
    QString mode = m_doc->isCollab() ? tr("collab") : tr("plain");
    QString modified = m_doc->isModified() ? tr("*") : QString();
    setWindowTitle(QStringLiteral("%1%2 [%3] — collabedit")
                       .arg(modified, name, mode));

    QString path = m_doc->path().isEmpty() ? tr("(no file)") : m_doc->path();
    m_statusLabel->setText(QStringLiteral("%1  |  %2  |  replica: %3")
                               .arg(path, mode, QString::fromStdString(m_replicaName)));

    bool canEnable = !m_doc->isCollab() && !m_doc->path().isEmpty();
    m_actEnableCollab->setEnabled(canEnable);
    m_enableCollabBtn->setVisible(canEnable);
    m_actSaveAs->setEnabled(!m_doc->isCollab());
}

void MainWindow::openFile(const QString &path) {
    QString err = m_doc->open(path);
    if (!err.isEmpty()) {
        QMessageBox::warning(this, tr("Open failed"), err);
    }
    updateTitleAndStatus();
}

void MainWindow::onFileNew() {
    QString err = m_doc->newDoc();
    if (!err.isEmpty()) QMessageBox::warning(this, tr("New failed"), err);
    updateTitleAndStatus();
}

void MainWindow::onFileOpen() {
    QString path = QFileDialog::getOpenFileName(this, tr("Open file"));
    if (path.isEmpty()) return;
    openFile(path);
}

void MainWindow::onFileSave() {
    QString err = m_doc->save();
    if (!err.isEmpty()) QMessageBox::warning(this, tr("Save failed"), err);
    updateTitleAndStatus();
}

void MainWindow::onFileSaveAs() {
    if (m_doc->isCollab()) return;
    QString path = QFileDialog::getSaveFileName(this, tr("Save as"));
    if (path.isEmpty()) return;
    QString err = m_doc->saveAs(path);
    if (!err.isEmpty()) QMessageBox::warning(this, tr("Save As failed"), err);
    updateTitleAndStatus();
}

void MainWindow::onFileClose() {
    m_doc->closeDoc();
    setCentralWidget(m_doc->widget());
    updateTitleAndStatus();
}

void MainWindow::onEnableCollab() {
    auto reply = QMessageBox::question(
        this, tr("Enable Collab"),
        tr("Enable collaborative editing on '%1'?\n\n"
           "This creates a sidecar folder next to the file. The sidecar "
           "must be inside a Syncthing-shared folder for remote peers to "
           "join.")
            .arg(m_doc->displayName()),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    QString err = m_doc->enableCollab();
    if (!err.isEmpty()) {
        QMessageBox::warning(this, tr("Enable Collab failed"), err);
    } else {
        setCentralWidget(m_doc->widget());
    }
    updateTitleAndStatus();
}

void MainWindow::onAbout() {
    QMessageBox::about(this, tr("About collabedit"),
        tr("collabedit — minimal CRDT collaborative editor\n\n"
           "Identity: %1\nReplica: %2")
            .arg(QString::fromStdString(m_identity.display_name),
                 QString::fromStdString(m_replicaName)));
}

void MainWindow::closeEvent(QCloseEvent *event) {
    m_doc->closeDoc();
    event->accept();
}

} // namespace CollabEdit
```

- [ ] **Step 3: Write a placeholder Document.h/.cpp (filled out in Task 7)**

Create `app/collabedit/Document.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"

#include <QObject>
#include <QString>
#include <memory>
#include <string>

class QWidget;
class QPlainTextEdit;

namespace CollabEdit {

/// Document holds either a Plain QPlainTextEdit or a CollabPane,
/// transitions between them, and exposes a uniform API to MainWindow.
///
/// State is "no document" | "Plain (file path)" | "Collab (sidecar)".
class Document : public QObject {
    Q_OBJECT
public:
    Document(CollabText::Identity::Identity identity,
             std::string replica_name,
             QObject *parent = nullptr);
    ~Document() override;

    QWidget *widget() const;            ///< Current editor widget; never null
    QString  displayName() const;       ///< Filename or "(untitled)"
    QString  path() const;              ///< Empty if no file open
    bool     isCollab() const;
    bool     isModified() const;

    QString newDoc();                   ///< Empty Plain doc; returns error or ""
    QString open(const QString &path);  ///< Returns error or ""
    QString save();                     ///< Returns error or ""
    QString saveAs(const QString &path);///< Plain mode only
    QString enableCollab();             ///< Plain → Collab; returns error or ""
    void    closeDoc();                 ///< Departs presence, closes; falls back to empty Plain.

signals:
    void changed();                     ///< Emitted on any state change

private:
    void emitChanged() { emit changed(); }

    CollabText::Identity::Identity m_identity;
    std::string m_replicaName;

    QString m_path;
    bool    m_collab = false;

    // Plain mode
    QPlainTextEdit *m_plainEdit = nullptr;
    QWidget *m_emptyWidget = nullptr;

    // Currently-presented widget (one of m_plainEdit, m_emptyWidget,
    // or a future CollabPane).
    QWidget *m_currentWidget = nullptr;
};

} // namespace CollabEdit
```

Create `app/collabedit/Document.cpp` (Plain-only implementation; Collab-mode methods stubbed to return "not implemented yet" — filled in Task 8):

```cpp
#include "Document.h"

#include <QFile>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QTextStream>
#include <QVBoxLayout>
#include <QLabel>

namespace CollabEdit {

Document::Document(CollabText::Identity::Identity identity,
                   std::string replica_name,
                   QObject *parent)
    : QObject(parent)
    , m_identity(std::move(identity))
    , m_replicaName(std::move(replica_name))
{
    m_emptyWidget = new QLabel(tr("(no document open — use File → New or Open...)"));
    static_cast<QLabel*>(m_emptyWidget)->setAlignment(Qt::AlignCenter);
    m_currentWidget = m_emptyWidget;
}

Document::~Document() {
    delete m_plainEdit;
    delete m_emptyWidget;
}

QWidget *Document::widget() const { return m_currentWidget; }

QString Document::displayName() const {
    if (m_path.isEmpty()) return tr("(untitled)");
    return QFileInfo(m_path).fileName();
}

QString Document::path() const { return m_path; }
bool Document::isCollab() const { return m_collab; }

bool Document::isModified() const {
    if (m_plainEdit) return m_plainEdit->document()->isModified();
    return false;
}

QString Document::newDoc() {
    closeDoc();
    m_path.clear();
    m_collab = false;
    m_plainEdit = new QPlainTextEdit;
    m_currentWidget = m_plainEdit;
    QObject::connect(m_plainEdit->document(), &QTextDocument::contentsChanged,
                     this, &Document::emitChanged);
    emit changed();
    return {};
}

QString Document::open(const QString &path) {
    closeDoc();
    m_path = path;
    m_collab = false;

    QFile f(path);
    QString content;
    if (f.exists()) {
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return tr("cannot read %1: %2").arg(path, f.errorString());
        QTextStream in(&f);
        content = in.readAll();
    }
    m_plainEdit = new QPlainTextEdit;
    m_plainEdit->setPlainText(content);
    m_plainEdit->document()->setModified(false);
    m_currentWidget = m_plainEdit;
    QObject::connect(m_plainEdit->document(), &QTextDocument::contentsChanged,
                     this, &Document::emitChanged);
    emit changed();
    return {};
}

QString Document::save() {
    if (m_plainEdit) {
        if (m_path.isEmpty()) return tr("no file path; use Save As");
        QFile f(m_path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return tr("cannot write %1: %2").arg(m_path, f.errorString());
        QTextStream out(&f);
        out << m_plainEdit->toPlainText();
        m_plainEdit->document()->setModified(false);
        emit changed();
        return {};
    }
    return tr("no document");
}

QString Document::saveAs(const QString &path) {
    if (!m_plainEdit) return tr("no document");
    m_path = path;
    return save();
}

QString Document::enableCollab() {
    return tr("Collab mode not implemented yet");
}

void Document::closeDoc() {
    if (m_plainEdit) {
        m_plainEdit->deleteLater();
        m_plainEdit = nullptr;
    }
    m_path.clear();
    m_collab = false;
    m_currentWidget = m_emptyWidget;
    emit changed();
}

} // namespace CollabEdit
```

- [ ] **Step 4: Write main.cpp**

Create `app/collabedit/main.cpp`:

```cpp
#include "MainWindow.h"

#include "collabtext/IdentityStore.h"
#include "ui/IdentitySetupDialog.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QHostInfo>
#include <QStandardPaths>

#include <string>

using namespace CollabText;

static std::string deriveReplicaName(const Identity::Identity &id) {
    std::string idPrefix = id.identity_id.size() >= 8
        ? id.identity_id.substr(0, 8)
        : id.identity_id;
    std::string host = QHostInfo::localHostName().toStdString();
    if (host.empty()) host = "host";
    return idPrefix + "-" + host;
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("collabedit");
    QApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Minimal CRDT collaborative editor.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "File to open (optional).");
    parser.process(app);

    // Identity: load or prompt
    QString configRoot = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configRoot);
    Identity::IdentityStore store(configRoot.toStdString());

    auto loaded = store.load();
    Identity::Identity identity;
    if (loaded) {
        identity = *loaded;
    } else {
        Ui::IdentitySetupDialog dlg(store);
        if (dlg.exec() != QDialog::Accepted) return 0;
        identity = dlg.identity();
    }

    std::string replicaName = deriveReplicaName(identity);

    CollabEdit::MainWindow win(identity, replicaName);
    win.show();

    auto args = parser.positionalArguments();
    if (!args.isEmpty()) {
        win.openFile(args.first());
    }

    return app.exec();
}
```

- [ ] **Step 5: Wire up CMake**

Replace contents of `app/collabedit/CMakeLists.txt`:

```cmake
qt_add_executable(collabedit
    main.cpp
    MainWindow.cpp
    MainWindow.h
    Document.cpp
    Document.h
)
target_link_libraries(collabedit PRIVATE Qt6::Widgets CollabText::CollabText)
target_include_directories(collabedit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/collabtext/src)
```

- [ ] **Step 6: Build and run**

Run: `cmake --build build-dev --target collabedit 2>&1 | tail -10`
Expected: build succeeds.

Manual smoke (non-blocking, just visual):
- `./build-dev/app/collabedit/collabedit /tmp/foo.md` opens a window in Plain mode.
- File → New, File → Open work; Save writes the file.
- Document → Enable Collab shows confirm; clicking Yes shows the "not implemented yet" warning.
- Window title shows `<filename> [plain] — collabedit`.

- [ ] **Step 7: Commit**

```bash
git add app/collabedit/main.cpp app/collabedit/MainWindow.h app/collabedit/MainWindow.cpp app/collabedit/Document.h app/collabedit/Document.cpp app/collabedit/CMakeLists.txt
git commit -m "feat(collabedit): skeleton app with Plain mode editor"
```

---

## Task 7: CollabPane — extracted minimal Collab editor

**Files:**
- Create: `app/collabedit/CollabPane.h`
- Create: `app/collabedit/CollabPane.cpp`
- Modify: `app/collabedit/CMakeLists.txt`

This is a stripped-down version of `EditorPane` from
`app/testapp/main.cpp` — same wiring pattern, but no gremlin, no
follow-mode, no chat, no comments. Reuses `CollabPlainTextEdit`,
`MultiCursorController`, and the per-pane sync/presence machinery.

- [ ] **Step 1: Write the header**

Create `app/collabedit/CollabPane.h`:

```cpp
#pragma once

#include "collabtext/Identity.h"
#include "collabtext/IdentityStore.h"
#include "collabtext/IdentityProjector.h"
#include "collabtext/PresenceManager.h"
#include "crdt/Buffer.h"
#include "crdt/FileSync.h"

#include <QWidget>

#include <filesystem>
#include <memory>
#include <string>

class QPlainTextDocumentLayout;
class QTextDocument;
class QTimer;

namespace CollabText::Ui {
class CollabPlainTextEdit;
class ParticipantListWidget;
}

namespace CollabEdit {

class CollabPane : public QWidget {
    Q_OBJECT
public:
    CollabPane(CollabText::Identity::Identity identity,
               std::string replica_name,
               std::filesystem::path sidecar_dir,
               const std::string &seed_text,
               QWidget *parent = nullptr);
    ~CollabPane() override;

    /// Current visible text (rendered from CRDT).
    std::string text() const;

    /// Stop sync timer, mark presence departed, flush remaining ops.
    void shutdown();

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded);
    void syncCycle();

private:
    void writePresence();
    void writeEphemeral();
    void applyRemoteCursors();
    uint32_t qtPosToByteOffset(int qtPos) const;
    int      byteOffsetToQtPos(uint32_t byteOffset) const;

    CollabText::Identity::Identity m_identity;
    std::string m_replicaName;

    CollabText::Crdt::Buffer m_buffer;
    CollabText::Crdt::FileSync m_sync;
    CollabText::Identity::PresenceManager m_presence;
    CollabText::Identity::IdentityProjector m_projector;

    CollabText::Ui::CollabPlainTextEdit *m_edit = nullptr;
    CollabText::Ui::ParticipantListWidget *m_participantList = nullptr;
    QTextDocument *m_qtDoc = nullptr;
    QTimer *m_syncTimer = nullptr;

    std::string m_sessionStarted;
    uint64_t m_ephemeralSeq = 0;
    bool m_syncing = false;
    bool m_shutdown = false;
};

} // namespace CollabEdit
```

- [ ] **Step 2: Write the implementation**

Create `app/collabedit/CollabPane.cpp`:

```cpp
#include "CollabPane.h"

#include "crdt/SeedOp.h"
#include "ui/CollabPlainTextEdit.h"
#include "ui/MultiCursorController.h"
#include "ui/ParticipantListWidget.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QPlainTextDocumentLayout>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <ctime>

using namespace CollabText;
using namespace CollabText::Crdt;
using namespace CollabText::Identity;
using namespace CollabText::Ui;

namespace CollabEdit {

namespace {
std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

uint16_t replicaIdFromName(const std::string &replica_name) {
    // Stable hash of replica_name into [1, 65535]. Avoid 0 (reserved
    // for the seed replica) and avoid collisions in practice via
    // string hash entropy.
    std::hash<std::string> h;
    uint64_t v = h(replica_name);
    uint16_t r = static_cast<uint16_t>(v % 65535) + 1;
    return r;
}
} // namespace

CollabPane::CollabPane(CollabText::Identity::Identity identity,
                       std::string replica_name,
                       std::filesystem::path sidecar_dir,
                       const std::string &seed_text,
                       QWidget *parent)
    : QWidget(parent)
    , m_identity(std::move(identity))
    , m_replicaName(std::move(replica_name))
    , m_buffer(replicaIdFromName(m_replicaName))
    , m_sync(m_buffer, sidecar_dir, m_replicaName)
    , m_presence(sidecar_dir, m_replicaName, m_identity.identity_id)
    , m_sessionStarted(now_iso8601())
{
    // Layout: editor on the left, participant list on the right.
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_edit = new CollabPlainTextEdit(this);
    m_qtDoc = new QTextDocument(this);
    m_qtDoc->setDocumentLayout(new QPlainTextDocumentLayout(m_qtDoc));
    m_qtDoc->setUndoRedoEnabled(false);
    m_edit->setDocument(m_qtDoc);
    root->addWidget(m_edit, 1);

    m_participantList = new ParticipantListWidget(this);
    m_participantList->setFixedWidth(220);
    root->addWidget(m_participantList);

    // 1) Apply seed deterministically.
    m_buffer.apply_ops({op_for_seed(seed_text)});

    // 2) Start FileSync (creates dirs, loads seq counters), then poll
    //    once to ingest any existing remote ops in the sidecar.
    m_sync.start();
    m_sync.poll();

    // 3) Render the resulting text into the QTextDocument as the
    //    initial content, then connect the change signal so future
    //    edits route to the buffer.
    m_syncing = true;
    m_qtDoc->setPlainText(QString::fromStdString(m_buffer.text()));
    m_syncing = false;

    connect(m_qtDoc, &QTextDocument::contentsChange,
            this, &CollabPane::onContentsChange);

    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(100);
    connect(m_syncTimer, &QTimer::timeout, this, &CollabPane::syncCycle);
    m_syncTimer->start();
}

CollabPane::~CollabPane() {
    shutdown();
}

std::string CollabPane::text() const { return m_buffer.text(); }

void CollabPane::shutdown() {
    if (m_shutdown) return;
    m_shutdown = true;
    if (m_syncTimer) m_syncTimer->stop();
    m_sync.poll();         // flush pending
    m_presence.depart();   // mark inactive
}

void CollabPane::onContentsChange(int position, int charsRemoved, int charsAdded) {
    if (m_syncing) return;

    std::string bufText = m_buffer.text();
    QString qBufText = QString::fromStdString(bufText);
    QString qtNow = m_qtDoc->toPlainText();

    uint32_t byteStart = qBufText.left(position).toUtf8().size();
    uint32_t byteEnd = byteStart;
    if (charsRemoved > 0) {
        byteEnd = qBufText.left(position + charsRemoved).toUtf8().size();
    }

    std::string inserted;
    if (charsAdded > 0) {
        QString insertedQt = qtNow.mid(position, charsAdded);
        inserted = insertedQt.toStdString();
    }

    auto op = m_buffer.apply_local_edit({{byteStart, byteEnd}}, {inserted});
    m_sync.push_local_op(op);
}

void CollabPane::syncCycle() {
    Global before = m_buffer.version();
    size_t applied = m_sync.poll();
    if (applied > 0) {
        auto edits = m_buffer.edits_since(before);
        m_syncing = true;
        QTextCursor cur(m_qtDoc);
        for (const auto &e : edits) {
            int qtStart = byteOffsetToQtPos(e.old_start);
            int qtEnd   = byteOffsetToQtPos(e.old_end);
            cur.setPosition(qtStart);
            cur.setPosition(qtEnd, QTextCursor::KeepAnchor);
            cur.insertText(QString::fromStdString(e.new_text));
        }
        m_syncing = false;
    }

    writePresence();
    writeEphemeral();
    applyRemoteCursors();
}

void CollabPane::writePresence() {
    Presence p;
    p.replica_id = m_replicaName;
    p.identity_id = m_identity.identity_id;
    p.device_name = m_replicaName;
    p.active = true;
    p.last_heartbeat = now_iso8601();
    p.session_started = m_sessionStarted;
    p.version_summary = m_buffer.version();
    m_presence.write_presence(p);
}

void CollabPane::writeEphemeral() {
    auto cursor = m_edit->textCursor();
    uint32_t bytePos = qtPosToByteOffset(cursor.position());
    uint32_t byteAnchor = qtPosToByteOffset(cursor.anchor());

    EphemeralState es;
    es.seq = ++m_ephemeralSeq;
    es.timestamp = now_iso8601();
    es.activity = "editing";
    auto posAnchor = m_buffer.anchor_at(bytePos, Bias::Right);
    auto selAnchor = m_buffer.anchor_at(byteAnchor, Bias::Left);
    es.cursors.push_back({selAnchor, posAnchor});

    m_presence.write_ephemeral(es);
}

void CollabPane::applyRemoteCursors() {
    auto remoteEphemerals = m_presence.read_remote_ephemerals();
    auto remotePresences  = m_presence.read_remote_presences();

    QList<RemoteCursor> cursors;
    std::vector<Presence> allPresences;

    for (const auto &[replicaId, p] : remotePresences) {
        allPresences.push_back(p);
    }
    // Add own presence to the participant list so the user sees self too.
    Presence self;
    self.replica_id = m_replicaName;
    self.identity_id = m_identity.identity_id;
    self.active = true;
    self.last_heartbeat = now_iso8601();
    allPresences.push_back(self);

    for (const auto &[replicaId, es] : remoteEphemerals) {
        // Find identity for this replica via presence
        std::optional<Identity::Identity> remoteIdentity;
        for (const auto &[prid, p] : remotePresences) {
            if (prid == replicaId) {
                auto maybe = m_projector.read(p.identity_id);
                if (maybe) remoteIdentity = *maybe;
                break;
            }
        }
        for (const auto &cp : es.cursors) {
            RemoteCursor rc;
            rc.bytePosition = m_buffer.resolve_anchor(cp.head);
            rc.byteAnchor   = m_buffer.resolve_anchor(cp.anchor);
            if (remoteIdentity) {
                rc.color = QColor(QString::fromStdString(remoteIdentity->color));
                rc.label = QString::fromStdString(remoteIdentity->display_name);
                rc.identityId = QString::fromStdString(remoteIdentity->identity_id);
            } else {
                rc.color = QColor(Qt::gray);
                rc.label = QString::fromStdString(replicaId);
            }
            rc.cursorVersion = (quint64(cp.head.replica_id) << 32) | cp.head.char_value;
            cursors.append(rc);
        }
    }
    m_edit->multiCursorController()->setRemoteCursors(cursors);

    auto identities = m_projector.read_all();
    m_participantList->updateParticipants(identities, allPresences);
}

uint32_t CollabPane::qtPosToByteOffset(int qtPos) const {
    QString docText = m_qtDoc->toPlainText();
    return docText.left(qMin(qtPos, docText.length())).toUtf8().size();
}

int CollabPane::byteOffsetToQtPos(uint32_t byteOffset) const {
    QString docText = m_qtDoc->toPlainText();
    QByteArray utf8 = docText.toUtf8();
    uint32_t clamped = qMin(byteOffset, static_cast<uint32_t>(utf8.size()));
    return QString::fromUtf8(utf8.data(), clamped).length();
}

} // namespace CollabEdit
```

- [ ] **Step 3: Update CMake**

Edit `app/collabedit/CMakeLists.txt`:

```cmake
qt_add_executable(collabedit
    main.cpp
    MainWindow.cpp
    MainWindow.h
    Document.cpp
    Document.h
    CollabPane.cpp
    CollabPane.h
)
target_link_libraries(collabedit PRIVATE Qt6::Widgets CollabText::CollabText)
target_include_directories(collabedit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/collabtext/src)
```

- [ ] **Step 4: Build (no smoke yet — Document.cpp doesn't use CollabPane)**

Run: `cmake --build build-dev --target collabedit 2>&1 | tail -20`
Expected: builds. CollabPane is currently unused but compiles.

- [ ] **Step 5: Commit**

```bash
git add app/collabedit/CollabPane.h app/collabedit/CollabPane.cpp app/collabedit/CMakeLists.txt
git commit -m "feat(collabedit): CollabPane — Buffer/FileSync/Presence editor pane"
```

---

## Task 8: Document — Plain↔Collab transition + Enable Collab + open existing sidecar

**Files:**
- Modify: `app/collabedit/Document.h`
- Modify: `app/collabedit/Document.cpp`

- [ ] **Step 1: Update Document.h to manage CollabPane and sidecar paths**

Edit `app/collabedit/Document.h`. Replace its entire contents with:

```cpp
#pragma once

#include "collabtext/Identity.h"

#include <QObject>
#include <QString>

#include <filesystem>
#include <memory>
#include <string>

class QWidget;
class QPlainTextEdit;

namespace CollabEdit {

class CollabPane;

class Document : public QObject {
    Q_OBJECT
public:
    Document(CollabText::Identity::Identity identity,
             std::string replica_name,
             QObject *parent = nullptr);
    ~Document() override;

    QWidget *widget() const;
    QString  displayName() const;
    QString  path() const;
    bool     isCollab() const;
    bool     isModified() const;

    QString newDoc();
    QString open(const QString &path);
    QString save();
    QString saveAs(const QString &path);
    QString enableCollab();
    void    closeDoc();

signals:
    void changed();

private:
    std::filesystem::path sidecarPath(const QString &filePath) const;
    QString openInPlainMode(const QString &path, QString *outContent = nullptr);
    QString openInCollabMode(const QString &path);

    void emitChanged() { emit changed(); }

    CollabText::Identity::Identity m_identity;
    std::string m_replicaName;

    QString m_path;
    bool    m_collab = false;

    QPlainTextEdit *m_plainEdit = nullptr;
    CollabPane     *m_collabPane = nullptr;
    QWidget        *m_emptyWidget = nullptr;
    QWidget        *m_currentWidget = nullptr;
};

} // namespace CollabEdit
```

- [ ] **Step 2: Update Document.cpp with full Plain/Collab logic**

Replace `app/collabedit/Document.cpp` with:

```cpp
#include "Document.h"
#include "CollabPane.h"

#include "crdt/SidecarManifest.h"

#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTextStream>
#include <QUuid>
#include <QDateTime>

#include <chrono>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;
using namespace CollabText::Crdt;

namespace CollabEdit {

namespace {
std::string now_iso8601_utc() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

QString readFileToString(const QString &path, QString *err) {
    QFile f(path);
    if (!f.exists()) return QString();
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = QObject::tr("cannot read %1: %2").arg(path, f.errorString());
        return QString();
    }
    QTextStream in(&f);
    return in.readAll();
}

QString writeStringToFile(const QString &path, const QString &content) {
    // Atomic-ish: write to .tmp, rename.
    QFile tmp(path + ".tmp");
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return QObject::tr("cannot write %1: %2").arg(path + ".tmp", tmp.errorString());
    QTextStream out(&tmp);
    out << content;
    tmp.close();
    QFile::remove(path);
    if (!QFile::rename(path + ".tmp", path))
        return QObject::tr("rename failed for %1").arg(path);
    return {};
}
} // namespace

Document::Document(CollabText::Identity::Identity identity,
                   std::string replica_name,
                   QObject *parent)
    : QObject(parent)
    , m_identity(std::move(identity))
    , m_replicaName(std::move(replica_name))
{
    auto *lbl = new QLabel(tr("(no document open — use File → New or Open...)"));
    lbl->setAlignment(Qt::AlignCenter);
    m_emptyWidget = lbl;
    m_currentWidget = m_emptyWidget;
}

Document::~Document() {
    if (m_collabPane) m_collabPane->shutdown();
    delete m_plainEdit;
    delete m_collabPane;
    delete m_emptyWidget;
}

QWidget *Document::widget() const { return m_currentWidget; }

QString Document::displayName() const {
    if (m_path.isEmpty()) return tr("(untitled)");
    return QFileInfo(m_path).fileName();
}

QString Document::path() const { return m_path; }
bool Document::isCollab() const { return m_collab; }

bool Document::isModified() const {
    if (m_plainEdit) return m_plainEdit->document()->isModified();
    return false;  // Collab mode: snapshot is always "as of last save"
}

fs::path Document::sidecarPath(const QString &filePath) const {
    return fs::path(filePath.toStdString() + ".collab");
}

QString Document::newDoc() {
    closeDoc();
    m_path.clear();
    m_collab = false;
    m_plainEdit = new QPlainTextEdit;
    m_currentWidget = m_plainEdit;
    QObject::connect(m_plainEdit->document(), &QTextDocument::contentsChanged,
                     this, &Document::emitChanged);
    emit changed();
    return {};
}

QString Document::open(const QString &path) {
    closeDoc();
    m_path = path;

    auto sidecar = sidecarPath(path);
    if (fs::exists(sidecar / "manifest.json")) {
        return openInCollabMode(path);
    }
    return openInPlainMode(path);
}

QString Document::openInPlainMode(const QString &path, QString *outContent) {
    QString err;
    QString content = readFileToString(path, &err);
    if (!err.isEmpty()) return err;
    if (outContent) *outContent = content;

    m_collab = false;
    m_plainEdit = new QPlainTextEdit;
    m_plainEdit->setPlainText(content);
    m_plainEdit->document()->setModified(false);
    m_currentWidget = m_plainEdit;
    QObject::connect(m_plainEdit->document(), &QTextDocument::contentsChanged,
                     this, &Document::emitChanged);
    emit changed();
    return {};
}

QString Document::openInCollabMode(const QString &path) {
    auto sidecar = sidecarPath(path);
    auto manifest = read_manifest(sidecar / "manifest.json");
    if (!manifest)
        return tr("Sidecar exists but manifest is invalid: %1")
                   .arg(QString::fromStdString((sidecar / "manifest.json").string()));

    // Check for enrollment-conflict files
    for (const auto &entry : fs::directory_iterator(sidecar)) {
        std::string name = entry.path().filename().string();
        if (name.find("seed.sync-conflict") != std::string::npos
            || name.find("manifest.sync-conflict") != std::string::npos) {
            return tr("Enrollment conflict detected: extra '%1' file in sidecar. "
                      "Resolve manually before opening.")
                       .arg(QString::fromStdString(name));
        }
    }

    // Read seed.txt and verify its sha
    std::ifstream sf(sidecar / "seed.txt", std::ios::binary);
    if (!sf) return tr("Sidecar missing seed.txt");
    std::string seed((std::istreambuf_iterator<char>(sf)),
                      std::istreambuf_iterator<char>());
    if (sha256_hex(seed) != manifest->seed_sha256)
        return tr("seed.txt SHA mismatch — sidecar may be corrupt");

    m_collab = true;
    m_collabPane = new CollabPane(m_identity, m_replicaName,
                                  sidecar, seed);
    m_currentWidget = m_collabPane;
    emit changed();
    return {};
}

QString Document::save() {
    if (m_collab && m_collabPane) {
        if (m_path.isEmpty()) return tr("no file path");
        QString content = QString::fromStdString(m_collabPane->text());
        QString err = writeStringToFile(m_path, content);
        if (err.isEmpty()) emit changed();
        return err;
    }
    if (m_plainEdit) {
        if (m_path.isEmpty()) return tr("no file path; use Save As");
        QString err = writeStringToFile(m_path, m_plainEdit->toPlainText());
        if (err.isEmpty()) {
            m_plainEdit->document()->setModified(false);
            emit changed();
        }
        return err;
    }
    return tr("no document");
}

QString Document::saveAs(const QString &path) {
    if (m_collab) return tr("Save As is disabled in Collab mode");
    if (!m_plainEdit) return tr("no document");
    m_path = path;
    return save();
}

QString Document::enableCollab() {
    if (m_collab) return tr("already in Collab mode");
    if (!m_plainEdit) return tr("no document");
    if (m_path.isEmpty()) return tr("save the file first (need a path)");

    // 1. Save current text to file so seed.txt matches what's on disk.
    QString text = m_plainEdit->toPlainText();
    QString err = writeStringToFile(m_path, text);
    if (!err.isEmpty()) return err;

    // 2. Create sidecar dir.
    auto sidecar = sidecarPath(m_path);
    std::error_code ec;
    fs::create_directories(sidecar, ec);
    if (ec) return tr("cannot create sidecar: %1").arg(QString::fromStdString(ec.message()));

    // 3. Write seed.txt (skip if already exists with matching content).
    std::string seedStr = text.toStdString();
    auto seedPath = sidecar / "seed.txt";
    bool needWriteSeed = true;
    if (fs::exists(seedPath)) {
        std::ifstream sf(seedPath, std::ios::binary);
        std::string existing((std::istreambuf_iterator<char>(sf)),
                              std::istreambuf_iterator<char>());
        if (existing == seedStr) needWriteSeed = false;
    }
    if (needWriteSeed) {
        std::ofstream sf(seedPath, std::ios::binary | std::ios::trunc);
        if (!sf) return tr("cannot write seed.txt");
        sf.write(seedStr.data(), static_cast<std::streamsize>(seedStr.size()));
    }

    // 4. Write manifest.json (LAST — its presence signals enrolled).
    SidecarManifest manifest;
    manifest.schema_version = 1;
    manifest.doc_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    manifest.enrolled_at = now_iso8601_utc();
    manifest.original_filename = QFileInfo(m_path).fileName().toStdString();
    manifest.seed_sha256 = sha256_hex(seedStr);
    try {
        write_manifest(sidecar / "manifest.json", manifest);
    } catch (const std::exception &e) {
        return tr("cannot write manifest: %1").arg(e.what());
    }

    // 5. Tear down Plain mode, switch to Collab.
    if (m_plainEdit) {
        m_plainEdit->deleteLater();
        m_plainEdit = nullptr;
    }
    return openInCollabMode(m_path);
}

void Document::closeDoc() {
    if (m_collabPane) {
        m_collabPane->shutdown();
        m_collabPane->deleteLater();
        m_collabPane = nullptr;
    }
    if (m_plainEdit) {
        m_plainEdit->deleteLater();
        m_plainEdit = nullptr;
    }
    m_path.clear();
    m_collab = false;
    m_currentWidget = m_emptyWidget;
    emit changed();
}

} // namespace CollabEdit
```

- [ ] **Step 3: Build**

Run: `cmake --build build-dev --target collabedit 2>&1 | tail -20`
Expected: builds clean.

- [ ] **Step 4: Manual single-machine smoke**

```bash
# Terminal 1 — launches the app, opens a file, enables collab.
mkdir -p /tmp/collabedit-smoke
echo "hello world" > /tmp/collabedit-smoke/notes.md
./build-dev/app/collabedit/collabedit /tmp/collabedit-smoke/notes.md
# In the GUI: Document → Enable Collab → Yes
# Window title should switch to "notes.md [collab] — collabedit"
# Edit some text. Ctrl+S.
# cat /tmp/collabedit-smoke/notes.md  → reflects the edited text.
# ls /tmp/collabedit-smoke/notes.md.collab/  → contains manifest.json, seed.txt, replicas/
```

- [ ] **Step 5: Manual two-instance smoke (single machine, two processes, shared dir)**

Two instances on the same machine simulating two peers:

```bash
# Terminal 1
./build-dev/app/collabedit/collabedit /tmp/collabedit-smoke/notes.md
# Enable Collab. Type something.

# Terminal 2 (uses a DIFFERENT identity dir to act as a different peer)
HOME=/tmp/collabedit-bob-home XDG_CONFIG_HOME=/tmp/collabedit-bob-home/.config \
   ./build-dev/app/collabedit/collabedit /tmp/collabedit-smoke/notes.md
# This second instance prompts for identity (Bob). After setup, opens
# directly in Collab mode. Both windows should show the same text and
# converge on edits with a ~100ms delay.
```

Verify cursors and participant list update correctly.

- [ ] **Step 6: Commit**

```bash
git add app/collabedit/Document.h app/collabedit/Document.cpp
git commit -m "feat(collabedit): Plain/Collab state machine + Enable Collab + sidecar load"
```

---

## Task 9: Full test pass + cleanup

**Files:** none (verification only)

- [ ] **Step 1: Run the entire ctest suite**

Run: `ctest --test-dir build-dev --output-on-failure 2>&1 | tail -40`
Expected: all tests pass, including the two new ones (`tst_seed_op`, `tst_sidecar_manifest`) and all existing tests.

- [ ] **Step 2: Build release config to make sure nothing broke under optimization**

Run: `cmake --build build-release 2>&1 | tail -20`

If the release build dir doesn't exist or fails, this is best-effort; document it in the commit but don't block on it.

- [ ] **Step 3: Run the existing testapp to confirm reorganization didn't regress it**

Run: `timeout 3 ./build-dev/app/testapp/collabtext-testapp || true`
Expected: window opens, doesn't crash. (Killed by timeout; that's fine.)

- [ ] **Step 4: Verify the executables land where expected**

```bash
ls build-dev/app/collabedit/collabedit build-dev/app/testapp/collabtext-testapp
```

- [ ] **Step 5: Final commit if anything was tweaked**

If any small fixups were made:

```bash
git add -A
git commit -m "chore(collabedit): final cleanup after full-suite verification"
```

If nothing needed fixing, no commit; the previous tasks are the deliverable.

---

## Verification Checklist (at end of all tasks)

- [ ] `tst_seed_op` passes
- [ ] `tst_sidecar_manifest` passes
- [ ] Full `ctest` suite green
- [ ] `collabedit` builds and runs
- [ ] `collabtext-testapp` still builds and runs (existing demo intact)
- [ ] Two-instance manual smoke converges (Task 8 step 5)
- [ ] Sidecar layout matches spec §5: `manifest.json`, `seed.txt`, `replicas/`, `meta/`, `local/`, `.stignore`
- [ ] Enrollment writes `manifest.json` LAST (recovery property from spec §9)
