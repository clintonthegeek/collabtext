/// Sha256.h — a dependency-free SHA-256, because Core needs one and Core must
/// stay free of link-time dependencies.
///
/// Roadmap task A01 splits the library into components, and the rule it sets is
/// that compression and hash *dependencies* belong to FileTransport so that
/// Core can be built and linked by a consumer who has neither zstd nor a crypto
/// library. Two things in Core need a digest, though:
///
///   * `DerivedActor.h` derives a seed operation's and a simulated replica's
///     actor from a domain string and some bytes. It has to be a real hash: the
///     whole point is that two peers compute the same actor and two *different*
///     inputs never share one.
///   * Nothing else. Which is why this is 90 lines rather than a dependency.
///
/// The alternative considered was OpenSSL::Crypto in FileTransport, and it is
/// the better answer for a consumer who already links OpenSSL. It cannot be the
/// only answer: it would either put a crypto dependency back into Core, or leave
/// `DerivedActor` unable to hash at all. A01 also asks that hash dependencies
/// "export correctly for static consumers", and an in-tree FIPS 180-4
/// implementation has nothing to export.
///
/// The algorithm and its output are unchanged from what the file formats have
/// always used, so content identity is preserved: this is the same SHA-256 the
/// QCryptographicHash call produced, byte for byte. tst_sha256.cpp checks it
/// against the FIPS 180-4 vectors and against a digest produced by Qt.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace CollabText {

/// Lower-case hex SHA-256 of `data`.
[[nodiscard]] std::string sha256_hex(std::string_view data);

}  // namespace CollabText