/// tst_sha256.cpp — the in-tree SHA-256 is SHA-256.
///
/// Roadmap task A01 replaced QCryptographicHash with a dependency-free
/// implementation so that Core carries no hash *link* dependency. That is only
/// safe if the output is identical, because the seed hash, the simulator actor
/// derivation and every segment/manifest digest already in the tree depend on it
/// producing the same bytes they always did. A hash that is subtly wrong would
/// not fail loudly; it would change every derived actor and every stored digest
/// while still round-tripping within a run.
///
/// Three independent checks, deliberately: the FIPS 180-4 vectors for the
/// algorithm, Qt's implementation for agreement with what this replaced, and a
/// length sweep for the padding boundaries, which are where hand-written
/// implementations actually go wrong.

#include <QTest>

#include "core/Sha256.h"

#include <QCryptographicHash>
#include <QByteArray>

#include <random>
#include <string>

using CollabText::sha256_hex;

class TestSha256 : public QObject {
    Q_OBJECT
private slots:
    void fips_vectors() {
        // FIPS 180-4 / NIST examples.
        QCOMPARE(sha256_hex(""),
                 std::string("e3b0c44298fc1c149afbf4c8996fb924"
                             "27ae41e4649b934ca495991b7852b855"));
        QCOMPARE(sha256_hex("abc"),
                 std::string("ba7816bf8f01cfea414140de5dae2223"
                             "b00361a396177a9cb410ff61f20015ad"));
        QCOMPARE(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                 std::string("248d6a61d20638b8e5c026930c3e6039"
                             "a33ce45964ff2167f6ecedd419db06c1"));
    }

    void two_block_message() {
        // 56 bytes is the first length whose padding needs a second block, and
        // it is the boundary a naive implementation gets wrong.
        QCOMPARE(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                 std::string("248d6a61d20638b8e5c026930c3e6039"
                             "a33ce45964ff2167f6ecedd419db06c1"));
        const std::string fifty_six(56, 'a');
        QCOMPARE(sha256_hex(fifty_six), sha256_hex_via_qt(fifty_six));
    }

    void agrees_with_the_implementation_it_replaced() {
        // The strongest available evidence: same input, same bytes, from the
        // library that produced every digest already written.
        std::mt19937 rng(20261005);
        for (int trial = 0; trial < 512; ++trial) {
            std::string data;
            const size_t length = rng() % 300;
            data.reserve(length);
            for (size_t i = 0; i < length; ++i)
                data += static_cast<char>('a' + (rng() % 26));
            QCOMPARE(sha256_hex(data), sha256_hex_via_qt(data));
        }
    }

    void padding_boundaries() {
        // 55/56/57 and 63/64/65: the lengths where the length field stops or
        // starts fitting in the final block.
        for (size_t length : {1u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 121u}) {
            const std::string data(length, 'x');
            const std::string ours = sha256_hex(data);
            QCOMPARE(ours.size(), size_t(64));
            QCOMPARE(ours, sha256_hex_via_qt(data));
        }
    }

    void embedded_nul_bytes_are_hashed() {
        // The signature takes a string_view, so a caller can hash bytes that are
        // not text. Truncating at the first NUL would make two different file
        // contents hash the same.
        std::string with_nul("ab", 2);
        with_nul.push_back('\0');
        with_nul += "cd";
        QVERIFY(sha256_hex(with_nul) != sha256_hex("ab"));
        QCOMPARE(sha256_hex(with_nul), sha256_hex_via_qt(with_nul));
    }

private:
    static std::string sha256_hex_via_qt(const std::string& data) {
        return QCryptographicHash::hash(QByteArray::fromStdString(data),
                                        QCryptographicHash::Sha256)
            .toHex()
            .toStdString();
    }
};

QTEST_MAIN(TestSha256)
#include "tst_sha256.moc"