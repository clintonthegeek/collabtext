#include <QTest>
#include <QProcess>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <collabtext/Anchor.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>
#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "reference/BoundaryModel.h"
#include "TestActor.h"
#include "CodecTest.h"
#include <type_traits>
using namespace CollabText;
using namespace CollabText::Crdt;
static_assert(!std::is_default_constructible_v<TextBoundary>);
static_assert(std::is_copy_constructible_v<TextBoundary>);
static_assert(!std::is_convertible_v<TextBoundary,ListBoundary>);
namespace {
template<class T> ErrorCode refused_code(const Result<T>& result) {
    if (result.ok()) qFatal("expected typed refusal, received a successful value");
    return result.error().code();
}
PositionScope text_scope() { return {"doc","body",Primitive::text,1}; }
PositionScope list_scope() { return {"doc","structure",Primitive::list,1}; }
std::string name(Identity id) { return id.actor.to_string()+":"+std::to_string(id.counter); }
BoundaryReference::State reference(const Buffer& buffer, const std::set<std::string>& integrated) {
    BoundaryReference::State state{{"doc","body",BoundaryReference::Primitive::text,1},{},integrated};
    for (const auto& f:buffer.fragments()) {
        uint32_t scalar=0;
        // Independent UTF-8 width/count derivation, not the production scanner.
        const auto unicode=QString::fromUtf8(f.text).toUcs4();
        for (auto cp:unicode) {
            const uint32_t width=cp<=0x7f ? 1 : cp<=0x7ff ? 2 : cp<=0xffff ? 3 : 4;
            state.order.push_back({name({f.origin.actor,f.origin.counter+scalar++}),width,f.visible,{}});
        }
    }
    return state;
}
}
class TestPositions : public QObject {
    Q_OBJECT
private slots:
    void exact_affinity_edges_unicode_and_query_purity() {
        auto allocator=Test::test_counter(1);
        auto engine=checked_codec(CrdtEngine::create(allocator,text_scope()));
        int notifications=0, operations=0;
        engine->setOnChange([&]{++notifications;});
        engine->setOnLocalOp([&](const auto&){++operations;});
        auto empty_left=checked_codec(engine->boundary_at(0,Affinity::left));
        auto empty_right=checked_codec(engine->boundary_at(0,Affinity::right));
        engine->insert(0,"ab");
        QCOMPARE(checked_codec(engine->resolve(empty_left)).byte_offset,0u);
        QCOMPARE(checked_codec(engine->resolve(empty_right)).byte_offset,2u);
        auto left=checked_codec(engine->boundary_at(1,Affinity::left));
        auto right=checked_codec(engine->boundary_at(1,Affinity::right));
        auto start=checked_codec(engine->boundary_at(0,Affinity::left));
        auto end_left=checked_codec(engine->boundary_at(2,Affinity::left));
        auto end_right=checked_codec(engine->boundary_at(2,Affinity::right));
        const auto before=allocator->peek();
        QCOMPARE(checked_codec(engine->compare(left,right)),0);
        QCOMPARE(allocator->peek(),before);
        QCOMPARE(notifications,1); QCOMPARE(operations,1);
        engine->insert(1,"X");
        QCOMPARE(checked_codec(engine->resolve(left)).byte_offset,1u);
        QCOMPARE(checked_codec(engine->resolve(right)).byte_offset,2u);
        QCOMPARE(checked_codec(engine->compare(left,right)),-1);
        engine->insert(3,"Y");
        QCOMPARE(checked_codec(engine->resolve(end_left)).byte_offset,3u);
        QCOMPARE(checked_codec(engine->resolve(end_right)).byte_offset,4u);
        QCOMPARE(checked_codec(engine->resolve(start)).byte_offset,0u);
        auto unicode=checked_codec(CrdtEngine::create(Test::test_counter(2),text_scope()));
        unicode->insert(0,"a\xf0\x9f\x98\x80\xce\xb1");
        for (uint32_t offset:{0u,1u,5u,7u}) {
            auto token=unicode->boundary_at(offset,Affinity::left); QVERIFY(token.ok());
            QCOMPARE(checked_codec(unicode->resolve(token.value())).byte_offset,offset);
        }
        for (uint32_t offset:{2u,3u,4u,6u}) {
            auto token=unicode->boundary_at(offset,Affinity::left); QVERIFY(!token.ok());
            QCOMPARE(refused_code(token),ErrorCode::not_scalar_boundary);
        }
        auto off=unicode->boundary_at(8,Affinity::left); QVERIFY(!off.ok());
        QCOMPARE(refused_code(off),ErrorCode::off_end);
        auto invalid=unicode->boundary_at(0,static_cast<Affinity>(99)); QVERIFY(!invalid.ok());
        QCOMPARE(refused_code(invalid),ErrorCode::malformed);
    }
    void strict_deletion_original_token_undo_redo_and_independent_deletes() {
        auto a=checked_codec(CrdtEngine::create(Test::test_counter(1),text_scope()));
        auto b=checked_codec(CrdtEngine::create(Test::test_counter(2),text_scope()));
        std::vector<Operation> ops_a,ops_b;
        a->setOnLocalOp([&](const auto& op){ops_a.push_back(op);});
        b->setOnLocalOp([&](const auto& op){ops_b.push_back(op);});
        a->insert(0,"abc"); QVERIFY(CollabText::Test::apply_succeeded(b->apply_remote(ops_a.back())));
        auto caret=checked_codec(a->boundary_at(1,Affinity::left));
        auto following=checked_codec(a->attachment_at(1,Affinity::left,ReferentSide::following));
        auto preceding=checked_codec(a->attachment_at(1,Affinity::left,ReferentSide::preceding));
        auto bytes=checked_codec(encode_position(following));
        auto original=checked_codec(decode_position_as<TextAttachment>(bytes));
        a->remove(1,1);
        auto collapsed=checked_codec(a->resolve(caret));
        QCOMPARE(collapsed.byte_offset,1u); QVERIFY(collapsed.lost_right); QVERIFY(!collapsed.lost_left);
        QCOMPARE(refused_code(a->resolve(original)),ErrorCode::detached);
        QVERIFY(a->resolve(preceding).ok());
        QVERIFY(a->undo());
        QCOMPARE(checked_codec(a->resolve(original)).byte_offset,1u);
        QVERIFY(!checked_codec(a->resolve(original)).collapsed());
        QVERIFY(a->redo()); QCOMPARE(refused_code(a->resolve(original)),ErrorCode::detached);
        // B independently deletes b; undoing A's delete cannot restore B's.
        b->remove(1,1); QVERIFY(CollabText::Test::apply_succeeded(a->apply_remote(ops_b.back())));
        QVERIFY(a->undo()); QCOMPARE(refused_code(a->resolve(original)),ErrorCode::detached);
        QVERIFY(b->undo()); QVERIFY(CollabText::Test::apply_succeeded(a->apply_remote(ops_b.back())));
        QVERIFY(a->resolve(original).ok());
        auto edge=a->attachment_at(0,Affinity::left,ReferentSide::preceding); QVERIFY(!edge.ok());
        QCOMPARE(refused_code(edge),ErrorCode::malformed);
        QCOMPARE(checked_codec(encode_position(original)),bytes);
    }
    void scope_causal_delay_clear_and_moved_tokens_are_typed() {
        auto allocator=Test::test_counter(1);
        auto source=checked_codec(CrdtEngine::create(allocator,text_scope()));
        std::vector<Operation> ops;
        source->setOnLocalOp([&](const auto& op){ops.push_back(op);});
        auto empty=checked_codec(source->boundary_at(0,Affinity::left));
        source->insert(0,"ab");
        auto token=checked_codec(source->boundary_at(1,Affinity::left));
        for (auto axis:{0,1,2}) {
            auto scope=text_scope();
            auto error=ErrorCode::wrong_document;
            if(axis==0) scope.document="other";
            if(axis==1) {scope.stream="other";error=ErrorCode::wrong_stream;}
            if(axis==2) {scope.epoch=2;error=ErrorCode::wrong_epoch;}
            auto foreign=checked_codec(CrdtEngine::create(Test::test_counter(2),scope));
            auto result=foreign->resolve(token); QVERIFY(!result.ok());
            QCOMPARE(refused_code(result),error); QVERIFY(foreign->text().empty());
        }
        auto delayed=checked_codec(CrdtEngine::create(Test::test_counter(2),text_scope()));
        auto result=delayed->resolve(token); QVERIFY(!result.ok()); QCOMPARE(refused_code(result),ErrorCode::awaiting_state);
        QVERIFY(CollabText::Test::apply_succeeded(delayed->apply_remote(ops[0]))); QVERIFY(delayed->resolve(token).ok());
        const auto next=allocator->peek(); source->discard_local_view();
        QVERIFY(source->text().empty()); QCOMPARE(allocator->peek(),next);
        QCOMPARE(refused_code(source->resolve(empty)),ErrorCode::history_unavailable);
        QCOMPARE(refused_code(source->resolve(token)),ErrorCode::history_unavailable);
        QCOMPARE(refused_code(source->boundary_at(0,Affinity::left)),ErrorCode::history_unavailable);
        source->insert(0,"new"); QCOMPARE(refused_code(source->resolve(token)),ErrorCode::history_unavailable);
        auto moved=std::move(token); QCOMPARE(refused_code(delayed->resolve(token)),ErrorCode::malformed);
        QVERIFY(delayed->resolve(moved).ok());
        auto bad_scope=text_scope(); bad_scope.primitive=Primitive::list;
        QCOMPARE(refused_code(CrdtEngine::create(Test::test_counter(2),bad_scope)),ErrorCode::wrong_primitive);
        bad_scope=text_scope(); bad_scope.document.clear(); QVERIFY(!CrdtEngine::create(Test::test_counter(2),bad_scope).ok());
        bad_scope=text_scope(); bad_scope.stream=std::string(257,'x');
        QCOMPARE(refused_code(CrdtEngine::create(Test::test_counter(2),bad_scope)),ErrorCode::limit_exceeded);
        QVERIFY(!CrdtEngine::create({},text_scope()).ok());
        CrdtEngine unbound(Test::test_counter(4)); QCOMPARE(refused_code(unbound.boundary_at(0,Affinity::left)),ErrorCode::unknown_stream);
    }
    void list_boundaries_full_width_elements_duplicates_and_restoration() {
        auto allocator=Test::test_counter(1);
        auto list=checked_codec(IdListEngine::create(allocator,list_scope()));
        auto empty=checked_codec(list->boundary_at(0,Affinity::right));
        QVERIFY(list->try_insert_after(Anchor::min(),UINT64_MAX).ok());
        auto original=checked_codec(list->element_of(UINT64_MAX));
        QCOMPARE(checked_codec(list->resolve(original)).element_index,0u);
        auto left=checked_codec(list->boundary_at(0,Affinity::left));
        auto right=checked_codec(list->boundary_at(0,Affinity::right));
        const auto before=allocator->peek();
        QCOMPARE(refused_code(list->element_of(42)),ErrorCode::not_found);
        QCOMPARE(refused_code(list->boundary_at(2,Affinity::left)),ErrorCode::off_end);
        QCOMPARE(allocator->peek(),before);
        QVERIFY(list->try_insert_after(Anchor::min(),UINT64_MAX).ok());
        auto duplicate=checked_codec(list->element_of(UINT64_MAX));
        QCOMPARE(checked_codec(list->resolve(original)).element_index,1u);
        QCOMPARE(checked_codec(list->resolve(duplicate)).element_index,0u);
        QCOMPARE(checked_codec(list->resolve(left)).element_index,0u);
        QCOMPARE(checked_codec(list->resolve(right)).element_index,1u);
        QCOMPARE(checked_codec(list->resolve(empty)).element_index,2u);
        auto after=list->try_anchor_of(UINT64_MAX); QVERIFY(after.ok());
        QVERIFY(list->try_remove_at(after.value()).ok());
        QCOMPARE(refused_code(list->resolve(duplicate)),ErrorCode::detached);
        QVERIFY(list->undo().has_value()); QVERIFY(list->resolve(duplicate).ok());
        QVERIFY(list->redo().has_value()); QCOMPARE(refused_code(list->resolve(duplicate)),ErrorCode::detached);
        auto foreign_scope=list_scope(); foreign_scope.stream="different";
        auto foreign=checked_codec(IdListEngine::create(Test::test_counter(2),foreign_scope));
        QCOMPARE(refused_code(foreign->resolve(original)),ErrorCode::wrong_stream);
        list->discard_local_view(); QCOMPARE(refused_code(list->resolve(empty)),ErrorCode::history_unavailable);
        QCOMPARE(refused_code(list->element_of(UINT64_MAX)),ErrorCode::history_unavailable);
    }
    void differential_native_text_histories() {
        uint64_t checks=0;
        for (const std::string base:{"", "ab", "a\xf0\x9f\x98\x80\xce\xb1", "abcdef"}) {
            const auto scalars=QString::fromUtf8(base).toUcs4();
            for(size_t insertion=0; insertion<=scalars.size(); ++insertion) {
                auto engine=checked_codec(CrdtEngine::create(Test::test_counter(1),text_scope()));
                Buffer mirror(Test::test_counter(2)); std::set<std::string> integrated;
                int ops=0, notifications=0;
                engine->setOnLocalOp([&](const auto& op){
                    const auto result=mirror.apply_ops({op});
                    if(result[0].kind()!=ApplyOutcomeKind::applied) qFatal("mirror rejected actual operation");
                    integrated.insert(name(op_identity(op))); ++ops;
                });
                engine->setOnChange([&]{++notifications;});
                if(!base.empty()) engine->insert(0,base);
                auto state=reference(mirror,integrated);
                struct Pair { TextBoundary actual; BoundaryReference::Token model; };
                std::vector<Pair> tokens;
                uint32_t offset=0;
                for(size_t gap=0;gap<=scalars.size();++gap) {
                    for(auto affinity:{Affinity::left,Affinity::right}) {
                        const auto bias=affinity==Affinity::left ? BoundaryReference::Affinity::left : BoundaryReference::Affinity::right;
                        auto model=BoundaryReference::mint(state,offset,bias); QVERIFY(model.has_value());
                        tokens.push_back({checked_codec(engine->boundary_at(offset,affinity)),*model});
                    }
                    if(gap<scalars.size()) {
                        auto cp=scalars[gap]; offset+=cp<=0x7f ? 1 : cp<=0x7ff ? 2 : cp<=0xffff ? 3 : 4;
                    }
                }
                auto verify=[&] {
                    const auto state=reference(mirror,integrated);
                    const auto before_ops=ops,before_notifications=notifications;
                    const auto before_text=engine->text(); const auto before_version=mirror.version();
                    for(const auto& token:tokens) {
                        auto expected=BoundaryReference::resolve(state,token.model);
                        auto actual=engine->resolve(token.actual);
                        if(expected.status!=BoundaryReference::Status::resolved || !actual.ok()) {
                            qWarning("position mismatch base=%s insertion=%zu",base.c_str(),insertion); return false;
                        }
                        const auto& value=actual.value();
                        if(value.byte_offset!=expected.offset || value.lost_left!=expected.lost_left || value.lost_right!=expected.lost_right) return false;
                        ++checks;
                    }
                    return ops==before_ops && notifications==before_notifications && engine->text()==before_text && mirror.version()==before_version;
                };
                QVERIFY(verify());
                int unit=0; for(size_t n=0;n<insertion;++n) unit+=scalars[n]>0xffff ? 2 : 1;
                engine->insert(unit,"X"); QVERIFY(verify());
                const auto current=QString::fromUtf8(engine->text()).toUcs4();
                engine->remove(0,current[0]>0xffff ? 2 : 1); QVERIFY(verify());
                QVERIFY(engine->undo()); QVERIFY(verify());
                QVERIFY(engine->redo()); QVERIFY(verify());
                QVERIFY(engine->undo()); QVERIFY(engine->undo()); QVERIFY(verify());
                QVERIFY(engine->redo()); QVERIFY(verify());
            }
        }
        qInfo("%llu native/model text resolutions",static_cast<unsigned long long>(checks));
    }
    void concurrent_replicas_post_merge_and_hidden_mint() {
        auto seed=checked_codec(CrdtEngine::create(Test::test_counter(3),text_scope()));
        auto a=checked_codec(CrdtEngine::create(Test::test_counter(1),text_scope()));
        auto b=checked_codec(CrdtEngine::create(Test::test_counter(2),text_scope()));
        Operation initial,op_a,op_b;
        seed->setOnLocalOp([&](const auto& op){initial=op;}); seed->insert(0,"ab");
        QVERIFY(CollabText::Test::apply_succeeded(a->apply_remote(initial))); QVERIFY(CollabText::Test::apply_succeeded(b->apply_remote(initial)));
        auto left=checked_codec(a->boundary_at(1,Affinity::left));
        auto right=checked_codec(a->boundary_at(1,Affinity::right));
        a->setOnLocalOp([&](const auto& op){op_a=op;}); b->setOnLocalOp([&](const auto& op){op_b=op;});
        a->insert(1,"X"); b->insert(1,"Y");
        const auto inserted_a=op_a,inserted_b=op_b;
        QVERIFY(CollabText::Test::apply_succeeded(a->apply_remote(inserted_b))); QVERIFY(CollabText::Test::apply_succeeded(b->apply_remote(inserted_a)));
        QCOMPARE(a->text(),b->text()); QCOMPARE(a->text(),std::string("aXYb"));
        QCOMPARE(checked_codec(a->resolve(left)).byte_offset,1u);
        QCOMPARE(checked_codec(b->resolve(left)).byte_offset,1u);
        QCOMPARE(checked_codec(a->resolve(right)).byte_offset,3u);
        QCOMPARE(checked_codec(b->resolve(right)).byte_offset,3u);
        auto post=checked_codec(a->boundary_at(2,Affinity::left));
        auto strict=checked_codec(a->attachment_at(2,Affinity::left,ReferentSide::preceding));
        QVERIFY(a->undo()); QVERIFY(CollabText::Test::apply_succeeded(b->apply_remote(op_a)));
        QCOMPARE(checked_codec(a->resolve(post)).byte_offset,1u);
        QVERIFY(checked_codec(a->resolve(post)).lost_left);
        QCOMPARE(refused_code(a->resolve(strict)),ErrorCode::detached);
        auto hidden_left=checked_codec(a->boundary_at(1,Affinity::left));
        auto hidden_right=checked_codec(a->boundary_at(1,Affinity::right));
        QVERIFY(a->redo()); QVERIFY(CollabText::Test::apply_succeeded(b->apply_remote(op_a)));
        QCOMPARE(checked_codec(a->resolve(hidden_left)).byte_offset,1u);
        QCOMPARE(checked_codec(a->resolve(hidden_right)).byte_offset,2u);
        QVERIFY(a->resolve(strict).ok());
        QCOMPARE(checked_codec(a->resolve(post)).byte_offset,2u);
        QCOMPARE(checked_codec(b->resolve(post)).byte_offset,2u);
    }
    void differential_native_list_histories() {
        uint64_t checks=0;
        for(uint32_t count=0;count<=4;++count) for(uint32_t inserted_gap=0;inserted_gap<=count;++inserted_gap) {
            auto engine=checked_codec(IdListEngine::create(Test::test_counter(1),list_scope()));
            IdList mirror(Test::test_counter(2)); std::set<std::string> integrated;
            engine->set_on_local_op([&](const auto& op){
                const auto result=mirror.apply_ops({op});
                if(result[0].kind()!=ApplyOutcomeKind::applied) qFatal("list mirror rejected actual op");
                integrated.insert(name(idlist_op_identity(op)));
            });
            for(uint32_t n=0;n<count;++n) QVERIFY(engine->try_insert_after(Anchor::min(),uint64_t(n)+1).ok());
            auto state=[&] {
                BoundaryReference::State out{{"doc","structure",BoundaryReference::Primitive::list,1},{},integrated};
                for(const auto& entry:mirror.entries()) out.order.push_back({name(entry.origin),1,entry.visible,entry.id});
                return out;
            };
            struct Pair { ListBoundary actual; BoundaryReference::Token model; };
            std::vector<Pair> tokens;
            for(uint32_t gap=0;gap<=count;++gap) for(auto affinity:{Affinity::left,Affinity::right}) {
                const auto bias=affinity==Affinity::left ? BoundaryReference::Affinity::left : BoundaryReference::Affinity::right;
                auto model=BoundaryReference::mint(state(),gap,bias); QVERIFY(model.has_value());
                tokens.push_back({checked_codec(engine->boundary_at(gap,affinity)),*model});
            }
            auto verify=[&] {
                for(const auto& pair:tokens) {
                    auto expected=BoundaryReference::resolve(state(),pair.model);
                    auto result=engine->resolve(pair.actual);
                    if(!result.ok() || expected.status!=BoundaryReference::Status::resolved) return false;
                    if(result.value().element_index!=expected.offset || result.value().lost_left!=expected.lost_left || result.value().lost_right!=expected.lost_right) return false;
                    ++checks;
                }
                return true;
            };
            QVERIFY(verify());
            const auto ids=engine->ids();
            auto after=Anchor::min();
            if(inserted_gap) after=checked_codec(engine->try_anchor_of(ids[inserted_gap-1]));
            QVERIFY(engine->try_insert_after(after,UINT64_MAX).ok()); QVERIFY(verify());
            auto element=checked_codec(engine->element_of(UINT64_MAX));
            auto expected_element=BoundaryReference::element_of(state(),UINT64_MAX); QVERIFY(expected_element.has_value());
            QCOMPARE(checked_codec(engine->resolve(element)).element_index,
                     BoundaryReference::resolve_element(state(),*expected_element).offset);
            QVERIFY(engine->try_remove_at(checked_codec(engine->try_anchor_of(UINT64_MAX))).ok()); QVERIFY(verify());
            QCOMPARE(refused_code(engine->resolve(element)),ErrorCode::detached);
            QVERIFY(engine->undo().has_value()); QVERIFY(verify()); QVERIFY(engine->resolve(element).ok());
            QVERIFY(engine->redo().has_value()); QVERIFY(verify());
        }
        qInfo("%llu native/model list resolutions",static_cast<unsigned long long>(checks));
    }
    void fresh_process_original_token_replay() {
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        const auto file=tmp.path()+"/replay.bin";
        auto run=[&](const char* mode) {
            QProcess process; process.start(QString::fromUtf8(COLLABTEXT_POSITION_REPLAY_WORKER),{QString::fromUtf8(mode),file});
            if(!process.waitForFinished(15000) || process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0) {
                qWarning("worker failed: %s",process.readAllStandardError().constData()); return QByteArray{};
            }
            return process.readAllStandardOutput();
        };
        QCOMPARE(run("mint"),QByteArray("minted\n"));
        QFile artifact(file); QVERIFY(artifact.open(QIODevice::ReadOnly));
        const auto original=artifact.readAll(); artifact.close();
        QCOMPARE(run("resolve"),QByteArray("left=1 right=2 strict=2 list=0\n"));
        QCOMPARE(run("resolve"),QByteArray("left=1 right=2 strict=2 list=0\n"));
        QVERIFY(artifact.open(QIODevice::ReadOnly)); QCOMPARE(artifact.readAll(),original);
        qInfo("fresh-process artifact sha256=%s",QCryptographicHash::hash(original,QCryptographicHash::Sha256).toHex().constData());
    }
};
QTEST_GUILESS_MAIN(TestPositions)
#include "tst_positions.moc"
