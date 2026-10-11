#include <QTest>
#include <collabtext/Anchor.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>
#include "crdt/PositionPayload.h"
#include "crdt/PositionTraversal.h"
#include "CodecTest.h"
#include "TestActor.h"
using namespace CollabText;
namespace {
template<class T> ErrorCode refused_code(const Result<T>& result) {
    if (result.ok()) qFatal("expected typed refusal, received a successful value");
    return result.error().code();
}
std::string replace_byte(std::string bytes,size_t index,uint8_t value) { bytes.at(index)=static_cast<char>(value); return bytes; }
}
class TestAnchorCodec : public QObject {
    Q_OBJECT
private slots:
    void exact_empty_and_populated_binary_goldens() {
        auto text=checked_codec(CrdtEngine::create(Test::test_counter(1),{"d","s",Primitive::text,1}));
        auto empty=checked_codec(text->boundary_at(0,Affinity::left));
        const auto bytes=checked_codec(encode_position(empty));
        QCOMPARE(QByteArray::fromStdString(bytes).toHex(),QByteArray("4354505400010100000164000173000000000000000100000000"));
        text->insert(0,"a");
        auto token=checked_codec(text->boundary_at(1,Affinity::right));
        const auto populated=checked_codec(encode_position(token));
        const QByteArray golden("4354505400010101000164000173000000000000000101"
                                "000000000000000000000000000000010000000000000001"
                                "000001000000000000000000000000000000010000000000000002");
        QCOMPARE(QByteArray::fromStdString(populated).toHex(),golden);
        QCOMPARE(checked_codec(encode_position(checked_codec(decode_position(populated)))),populated);
    }
    void every_kind_all_truncated_prefixes_and_256_trailing_bytes() {
        auto text=checked_codec(CrdtEngine::create(Test::test_counter(1),{"doc","body",Primitive::text,1}));
        text->insert(0,"ab");
        auto list=checked_codec(IdListEngine::create(Test::test_counter(1),{"doc","structure",Primitive::list,1}));
        QVERIFY(list->try_insert_after(Crdt::Anchor::min(),UINT64_MAX).ok());
        std::vector<PositionToken> tokens;
        tokens.emplace_back(checked_codec(text->boundary_at(1,Affinity::left)));
        tokens.emplace_back(checked_codec(text->attachment_at(1,Affinity::right,ReferentSide::following)));
        tokens.emplace_back(checked_codec(list->boundary_at(1,Affinity::right)));
        tokens.emplace_back(checked_codec(list->element_of(UINT64_MAX)));
        for(const auto& token:tokens) {
            auto bytes=checked_codec(encode_position(token));
            for(size_t n=0;n<bytes.size();++n) QVERIFY(!decode_position(std::string_view(bytes).substr(0,n)).ok());
            for(unsigned n=0;n<256;++n) {
                auto extra=bytes+std::string(1,static_cast<char>(n));
                auto result=decode_position(extra); QVERIFY(!result.ok()); QCOMPARE(refused_code(result),ErrorCode::malformed);
            }
            auto decoded=checked_codec(decode_position(bytes));
            QCOMPARE(checked_codec(encode_position(decoded)),bytes);
            QCOMPARE(decoded.index(),token.index());
            bytes.assign(bytes.size(),'!'); // decoder-owned scope/frontier/items
            QVERIFY(encode_position(decoded).ok());
        }
        const auto wrong=decode_position_as<ListBoundary>(checked_codec(encode_position(tokens[0])));
        QVERIFY(!wrong.ok()); QCOMPARE(refused_code(wrong),ErrorCode::wrong_primitive);
        auto moved=std::move(tokens[0]);
        QVERIFY(!encode_position(tokens[0]).ok()); QVERIFY(encode_position(moved).ok());
    }
    void malformed_enums_context_frontier_identity_and_versions() {
        auto text=checked_codec(CrdtEngine::create(Test::test_counter(1),{"d","s",Primitive::text,1}));
        auto empty=checked_codec(encode_position(checked_codec(text->boundary_at(0,Affinity::left))));
        for(auto bytes:{replace_byte(empty,6,0),replace_byte(empty,6,5),replace_byte(empty,7,2),replace_byte(empty,9,0),
                        replace_byte(empty,8,1),replace_byte(empty,11,1),replace_byte(empty,21,0),replace_byte(empty,22,2)}) {
            auto decoded=decode_position(bytes); QVERIFY(!decoded.ok()); QCOMPARE(refused_code(decoded),ErrorCode::malformed);
        }
        auto newer=replace_byte(empty,5,3);
        QCOMPARE(refused_code(decode_position(newer)),ErrorCode::unsupported_version);
        QCOMPARE(refused_code(decode_position("future")),ErrorCode::unknown_format);
        QCOMPARE(refused_code(decode_position(std::string((64u<<10)+1,'x'))),ErrorCode::malformed);
        text->insert(0,"ab");
        auto token=checked_codec(text->boundary_at(1,Affinity::left));
        // Private builders are used only for targeted malformed writer/reader
        // fixtures. Public consumers cannot mutate or fabricate these values.
        const auto original=*detail::PositionAccess::get(token);
        auto bad=original; bad.left=bad.right;
        QVERIFY(!encode_position(detail::PositionAccess::make<TextBoundary>(bad)).ok());
        bad=original; bad.frontier={};
        QVERIFY(!encode_position(detail::PositionAccess::make<TextBoundary>(bad)).ok());
        bad=original; bad.left=Crdt::Identity(Test::actor(1),UINT64_MAX);
        QVERIFY(!encode_position(detail::PositionAccess::make<TextBoundary>(bad)).ok());
        const auto bytes=checked_codec(encode_position(token));
        // d/s have one byte each: left item begins at byte 23, counter at 39.
        auto zero_actor=bytes; zero_actor.replace(23,16,std::string(16,'\0'));
        QCOMPARE(refused_code(decode_position(zero_actor)),ErrorCode::malformed);
        auto max_counter=bytes; max_counter.replace(39,8,std::string(8,char(255)));
        QCOMPARE(refused_code(decode_position(max_counter)),ErrorCode::malformed);
        auto strict=checked_codec(text->attachment_at(1,Affinity::left,ReferentSide::following));
        auto strict_bytes=checked_codec(encode_position(strict));
        QCOMPARE(refused_code(decode_position(replace_byte(strict_bytes,22,3))),ErrorCode::malformed);
    }
    void frontier_order_limits_missing_history_and_inverted_fences() {
        detail::PositionPayload p{{std::string(256,'d'),std::string(256,'s'),Primitive::text,UINT64_MAX},detail::PositionKind::text_boundary};
        for(uint64_t n=1;n<=512;++n) p.frontier.observe({Test::actor64(n),UINT64_MAX-1});
        auto bytes=checked_codec(encode_position(detail::PositionAccess::make<TextBoundary>(p)));
        QVERIFY(bytes.size()<12874); QVERIFY(decode_position(bytes).ok());
        // Last fields: frontier count and exactly 512 (actor, head) entries.
        const size_t first=bytes.size()-512*24;
        auto duplicate=bytes; duplicate.replace(first+24,24,bytes.substr(first,24));
        QCOMPARE(refused_code(decode_position(duplicate)),ErrorCode::malformed);
        auto reversed=bytes; reversed.replace(first,24,bytes.substr(first+24,24)); reversed.replace(first+24,24,bytes.substr(first,24));
        QCOMPARE(refused_code(decode_position(reversed)),ErrorCode::malformed);
        auto zero=bytes; zero.replace(first+16,8,std::string(8,'\0'));
        QCOMPARE(refused_code(decode_position(zero)),ErrorCode::malformed);
        auto too_many=bytes; too_many[first-1]=1; // 0x0201 = 513 actors
        QCOMPARE(refused_code(decode_position(too_many)),ErrorCode::malformed);
        p.frontier.observe({Test::actor64(513),1});
        QCOMPARE(refused_code(encode_position(detail::PositionAccess::make<TextBoundary>(p))),ErrorCode::limit_exceeded);
        // Resolution cannot turn absent/inverted retained fences into an offset.
        p={{"doc","body",Primitive::text,1},detail::PositionKind::text_boundary};
        p.frontier.observe({Test::actor(1),3}); p.left=Crdt::Identity(Test::actor(1),1); p.right=Crdt::Identity(Test::actor(1),2);
        std::vector<detail::PositionItem> items{{*p.right,1,true,{}},{*p.left,1,true,{}}};
        QCOMPARE(refused_code(detail::resolve_boundary(p,items)),ErrorCode::history_unavailable);
        items.pop_back(); QCOMPARE(refused_code(detail::resolve_boundary(p,items)),ErrorCode::history_unavailable);
    }
};
QTEST_GUILESS_MAIN(TestAnchorCodec)
#include "tst_anchor_codec.moc"
