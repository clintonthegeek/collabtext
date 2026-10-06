// Independent Markdown/Markoff-shaped consumer fixture. It reaches the same
// installed library as the Wave-shaped fixture beside it and shares none of its
// composition, support code or expectations.
#include "Markoff.h"
#include <csignal>
#include <iostream>
using namespace Markoff;
namespace {
std::string quote(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}
std::string position(const Resolution& p) {
    return "{\"state\":" + quote(p.state) + ",\"offset\":" + (p.offset ? std::to_string(*p.offset) : "null") +
           ",\"error\":" + quote(p.error) + ",\"lost_left\":" + (p.lost_left ? "true" : "false") +
           ",\"lost_right\":" + (p.lost_right ? "true" : "false") + "}";
}
/// Heading, a paragraph with a non-ASCII scalar, and a two-line list item. The
/// literal bytes matter: a fixture that only round-trips ASCII proves less.
constexpr std::string_view kSample = "# Notes\n\nfirst body with \xc3\xa9\n\n- item one\n- item two\n";
std::string run(const std::string& name) {
    if (name == "exact_bytes") {
        auto doc = Document::parse(std::string(kSample), "notes", 1);
        require(doc.render() == kSample, "render did not reproduce the parsed bytes");
        std::string kinds = "[";
        const auto classified = doc.kinds();
        for (size_t n = 0; n < classified.size(); ++n) { if (n) kinds += ','; kinds += quote(classified[n]); }
        kinds += ']';
        return "{\"exact\":true,\"bytes\":" + std::to_string(kSample.size()) + ",\"blocks\":" +
               std::to_string(doc.block_count()) + ",\"kinds\":" + kinds + "}";
    }
    if (name == "transport_routing") {
        auto local = Document::parse(std::string(kSample), "notes", 1);
        // The peer takes the document over the wire rather than minting its own
        // history: exact plaintext, arriving as opaque frames.
        auto peer = Document::open(std::string(kSample), "notes", 2);
        size_t seeded = 0;
        for (auto id : {1u, 2u, 3u}) for (const auto& frame : local.at(id).outbox) {
            require(peer.receive(id, frame), "peer refused a seed frame"); ++seeded;
        }
        require(peer.render() == kSample, "a document assembled from frames did not reproduce the bytes");
        // Seed frames already queued before the wire existed, so exactly one
        // frame is the one edit below and nothing else rides along by accident.
        InMemoryWire wire;
        local.set_transport(&wire);
        local.at(1).engine->insert(0, "seeded ");
        require(wire.frame_count() == 1, "one local edit did not route exactly one frame");
        require(!wire.frames()[0].empty(), "routed frame is empty");
        for (const auto& frame : wire.frames()) require(peer.receive(1, frame), "peer refused a routed frame");
        require(peer.body(1) == local.body(1), "transported edit did not converge");
        require(peer.render() == "seeded # Notes\n\nfirst body with \xc3\xa9\n\n- item one\n- item two\n",
                "converged peer did not reproduce the transported bytes");
        // Transport is optional. A second document with none attached still
        // queues its own frame, and the attached wire sees nothing from it --
        // "no transport" is an observed absence, not an untested assumption.
        auto offline = Document::parse(std::string(kSample), "notes", 3);
        const auto queued_before = offline.at(1).outbox.size();
        const auto frames_before = wire.frame_count();
        offline.at(1).engine->insert(0, "seeded ");
        const auto queued = offline.at(1).outbox.size() - queued_before;
        require(queued == 1, "an untransported edit did not reach the consumer's own queue");
        require(wire.frame_count() == frames_before, "an untransported edit reached the wire");
        return std::string("{\"seed_frames\":") + std::to_string(seeded) + ",\"frames\":" +
               std::to_string(wire.frame_count()) + ",\"converged\":true,\"text\":" + quote(local.body(1)) +
               ",\"peer_text\":" + quote(peer.body(1)) + ",\"transported_render_exact\":true" +
               ",\"queued_without_transport\":" + std::to_string(queued) +
               ",\"wire_untouched\":true,\"payload_fields_read\":false}";
    }
    if (name == "caret_versus_strict") {
        auto doc = Document::parse("# Notes\n\nhello world\n", "notes", 1);
        auto caret = doc.mint(2, 6, Affinity::left, false);
        auto strict = doc.mint(2, 6, Affinity::right, true);
        // A non-adjacent edit must move neither reference.
        doc.at(2).engine->remove(0, 1);
        auto early_caret = doc.resolve(2, caret);
        auto early_strict = doc.resolve(2, strict);
        const auto after_edit = doc.render();
        // Deleting the strict referent also removes the caret's right neighbour.
        doc.at(2).engine->remove(5, 1);
        auto late_caret = doc.resolve(2, caret);
        auto late_strict = doc.resolve(2, strict);
        // The collapsed caret is still a usable answer, and the surviving caret
        // is not an intact strict attachment to the deleted character.
        require(late_caret.state == "Collapsed" && late_caret.offset.has_value(), "collapsed caret lost its offset");
        require(late_strict.offset.has_value() == false, "a detached strict attachment reported an offset");
        return std::string("{\"text\":") + position(late_caret) + ",\"after_edit\":" + position(early_caret) +
               ",\"after_edit_strict\":" + position(early_strict) + ",\"text_after_strict_delete\":" + quote(doc.body(2)) +
               ",\"strict_after_delete\":" + position(late_strict) + ",\"bytes_preserved\":" +
               (after_edit == "# Notes\n\nello world\n" ? "true" : "false") + "}";
    }
    if (name == "deleted_block_gone") {
        auto doc = Document::parse("# Notes\n\nhello world\n", "notes", 1);
        auto bytes = doc.mint(2, 6, Affinity::left, false);
        doc.erase(2);
        // A deleted block is Gone before decode, for malformed bytes too, and the
        // consumer keeps the original frame rather than discarding it.
        auto valid = doc.resolve(2, bytes);
        auto invalid = doc.resolve(2, "not-a-token");
        require(valid.state == "Gone" && invalid.state == "Gone", "deleted block did not answer Gone");
        return "{\"valid\":" + position(valid) + ",\"invalid\":" + position(invalid) +
               ",\"retained\":" + (bytes.empty() ? "false" : "true") + ",\"rendered\":" + quote(doc.render()) +
               ",\"blocks\":" + std::to_string(doc.block_count()) + "}";
    }
    throw std::runtime_error("unknown markdown case");
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    if (std::string(argv[1]) == "abnormal") { std::raise(SIGABRT); return 99; }
    try {
        std::cout << "{\"schema\":\"collabtext-markdown-worker/1\",\"case\":" << quote(argv[1])
                  << ",\"observed\":" << run(argv[1]) << "}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}