#include "reference/BoundaryModel.h"
#include "TestSeed.h"
#include "reference/BoundaryApiContract.h"
#include <type_traits>
static_assert(!std::is_default_constructible_v<CollabText::PositionApiTarget::TextBoundary>);
static_assert(std::is_copy_constructible_v<CollabText::PositionApiTarget::TextBoundary>);
static_assert(!std::is_convertible_v<CollabText::PositionApiTarget::TextBoundary, CollabText::PositionApiTarget::ListElement>);
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <map>
#include <numeric>

using namespace BoundaryReference;
namespace {
std::string str(const QJsonValue& v) { return v.toString().toStdString(); }
QJsonObject read(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) qFatal("Cannot read %s", qPrintable(path));
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) qFatal("Invalid fixture %s", qPrintable(path));
    return doc.object();
}
Context context(const QJsonObject& c, bool list) {
    return {str(c["document"]),str(c["stream"]),list ? Primitive::list : Primitive::text,uint64_t(c["epoch"].toInteger())};
}
std::optional<std::string> endpoint(const QJsonValue& v) {
    auto s = str(v); return s == "START" || s == "END" ? std::nullopt : std::optional(s);
}
}
class BoundaryModelTest : public QObject {
    Q_OBJECT
private slots:
    void exactFixtures();
    void typedFailuresAndListElements();
    void deletionUndoRedo();
    void postMergeAndHiddenMint();
    void boundedStates();
    void mutationCounterexamples();
};

void BoundaryModelTest::exactFixtures() {
    const QString root = QString::fromUtf8(COLLABTEXT_BOUNDARY_FIXTURE_DIR);
    auto inputs = read(root + "/external-position/model/retained-orders.json");
    QCOMPARE(inputs["schema"].toString(),QString("boundary-supplied-order/1"));
    auto orders = inputs["orders"].toObject();
    int fixtures = 0, placements = 0, comparisons = 0, mintChecks = 0;
    for (auto source = orders.begin(); source != orders.end(); ++source) {
        auto c = read(root + "/" + source.key());
        bool list = c["primitive"] == "list";
        State final{context(c["context"].toObject(),list),{}, {}};
        const std::set<std::string> seedHistory = c["initial"].toArray().isEmpty() ?
            std::set<std::string>{} : std::set<std::string>{"__initial_seed"};
        final.integrated = seedHistory;
        std::map<std::string,Item> items;
        std::map<std::string,std::string> owners;
        std::map<std::string,std::set<std::string>> closure;
        std::map<std::string,QJsonObject> events;
        auto add = [&](const QJsonObject& i, const std::string& owner) {
            const auto id = str(i["id"]);
            items.emplace(id, Item{id, list ? 1u : uint32_t(i["value"].toString().toUtf8().size()),true});
            if(list) items.at(id).application_value=uint64_t(i["value"].toInteger());
            owners[id] = owner;
        };
        for (auto i : c["initial"].toArray()) add(i.toObject(),{});
        for (auto e : c["operations"].toArray()) {
            auto event = e.toObject(); auto id = str(event["id"]);
            for (auto p : event["after"].toArray()) {
                auto parent = str(p); closure[id].insert(parent);
                closure[id].insert(closure[parent].begin(),closure[parent].end());
            }
            for (auto i : event["items"].toArray()) add(i.toObject(),id);
            events[id] = event; final.integrated.insert(id);
        }
        auto hidden = c["expected"].toObject()["hidden"].toArray();
        for (auto id : hidden) items.at(str(id)).visible = false;
        std::set<std::string> unique;
        QJsonArray visible;
        for (auto id : source.value().toArray()) {
            QVERIFY(unique.insert(str(id)).second);
            final.order.push_back(items.at(str(id)));
            if (items.at(str(id)).visible) visible.append(id);
        }
        QCOMPARE(unique.size(),items.size());
        QCOMPARE(visible,c["expected"].toObject()["order"].toArray());
        QVERIFY(valid(final));
        std::map<std::string,Token> tokens;
        for (auto t : c["tokens"].toArray()) {
            auto tok = t.toObject(); State mintState{final.context,{},seedHistory};
            for (auto p : tok["after"].toArray()) {
                auto parent = str(p); mintState.integrated.insert(parent);
                mintState.integrated.insert(closure[parent].begin(),closure[parent].end());
            }
            for (auto item : final.order) {
                if (!owners[item.id].empty() && !mintState.integrated.contains(owners[item.id])) continue;
                item.visible = true;
                for (auto id : mintState.integrated) {
                    if (seedHistory.contains(id)) continue;
                    auto e = events.at(id);
                    if (e["kind"] == "delete" && e["targets"].toArray().contains(QString::fromStdString(item.id))) item.visible = false;
                }
                mintState.order.push_back(item);
            }
            const auto pair = tok["between"].toArray();
            uint32_t offset = 0;
            if (auto left = endpoint(pair[0])) {
                bool found = false;
                for (auto& item : mintState.order) {
                    if (item.visible) offset += item.width;
                    if (item.id == *left) { found = true; break; }
                }
                QVERIFY(found);
            }
            auto minted = mint(mintState,offset,tok["affinity"] == "left" ? Affinity::left : Affinity::right);
            QVERIFY(minted.has_value());
            QVERIFY2(minted->left == endpoint(pair[0]) && minted->right == endpoint(pair[1]),
                     qPrintable(source.key()+": declared token is not a visible gap at mint"));
            tokens.emplace(str(tok["id"]),*minted); ++mintChecks;
        }
        auto expected = c["expected"].toObject();
        if(expected.contains("retained_order")) QCOMPARE(source.value().toArray(),expected["retained_order"].toArray());
        auto positions = expected["positions"].toObject();
        for (auto p = positions.begin(); p != positions.end(); ++p) {
            auto result = resolve(final,tokens.at(p.key().toStdString()));
            QCOMPARE(result.status,Status::resolved);
            QCOMPARE(result.offset,uint32_t(p.value().toObject()["offset"].toInteger()));
            QCOMPARE(result.collapsed(),p.value().toObject()["state"] == "collapsed"); ++placements;
        }
        for (auto e : c["expectations"].toArray()) {
            auto exp=e.toObject();
            if (exp["kind"] == "detached") {
                auto sides = c["strict_sides"].toObject();
                QVERIFY(sides.contains(exp["token"].toString()));
                QCOMPARE(resolve_strict(final,tokens.at(str(exp["token"])),sides[exp["token"].toString()] == "following").status,Status::detached);
            }
            if (exp["kind"] == "restored_after_undo") {
                auto restored=final;
                auto referent=str(exp["referent"]);
                for (auto& i:restored.order) if(i.id==referent) i.visible=true;
                auto result=resolve_strict(restored,tokens.at(str(exp["token"])),true);
                QCOMPARE(result.status,Status::resolved); QCOMPARE(result.offset,uint32_t(exp["offset"].toInteger()));
                QCOMPARE(result.collapsed(),false);
            }
        }
        for (auto cmp : expected["comparisons"].toArray()) {
            auto x = cmp.toObject(); auto relation = compare(final,tokens.at(str(x["left"])),tokens.at(str(x["right"])));
            QVERIFY(relation.has_value());
            QCOMPARE(*relation,x["order"]=="less" ? -1 : x["order"]=="greater" ? 1 : 0); ++comparisons;
        }
        for (auto request : c["invalid_requests"].toArray()) {
            auto r=request.toObject(); auto kind=str(r["kind"]);
            if (kind=="mint_offset" || kind=="mint_index")
                QVERIFY(!mint(final,uint32_t(r[kind=="mint_index" ? "index" : "offset"].toInteger()),Affinity::left));
            if (kind=="wrong_scope" && r.contains("axis")) {
                auto foreign=tokens.begin()->second; auto axis=str(r["axis"]); Status error;
                if(axis=="document") { foreign.context.document=str(r["value"]); error=Status::wrong_document; }
                else if(axis=="stream" || axis=="block") { foreign.context.stream=str(r["value"]); error=Status::wrong_stream; }
                else if(axis=="primitive") { foreign.context.primitive=Primitive::list; error=Status::wrong_primitive; }
                else { foreign.context.epoch=uint64_t(r["value"].toInteger()); error=Status::wrong_epoch; }
                QCOMPARE(resolve(final,foreign).status,error);
            }
            if(kind=="missing_list_id") {
                bool found=false;
                for(auto item : c["initial"].toArray()) if(item.toObject()["value"]==r["value"]) found=true;
                for(auto e : c["operations"].toArray()) for(auto item:e.toObject()["items"].toArray()) if(item.toObject()["value"]==r["value"]) found=true;
                QVERIFY(!found);
                QVERIFY(!element_of(final,uint64_t(r["value"].toInteger())));
            }
        }
        // Resolve the ORIGINAL handles against independently reconstructed final states
        // for each supplied causal arrival order, including duplicate receipts.
        for (auto arrival : c["arrival_orders"].toArray()) {
            State replay=final; replay.integrated=seedHistory;
            for (auto id:arrival.toArray()) { replay.integrated.insert(str(id)); replay.integrated.insert(str(id)); }
            for (auto& [id,t]:tokens) QCOMPARE(resolve(replay,t),resolve(final,t));
        }
        ++fixtures;
    }
    QCOMPARE(fixtures,21);
    qInfo("Supplied-order fixtures=%d minted-gaps=%d exact-positions=%d comparisons=%d",fixtures,mintChecks,placements,comparisons);
}

void BoundaryModelTest::typedFailuresAndListElements() {
    State state{{"doc","text",Primitive::text,1},{{"a",1,true},{"b",4,true}}, {"insert-ab"}};
    auto token=*mint(state,1,Affinity::left);
    auto missing=state; missing.integrated.clear(); missing.order.clear();
    QCOMPARE(resolve(missing,token).status,Status::awaiting_state);
    missing.integrated=state.integrated;
    QCOMPARE(resolve(missing,token).status,Status::unavailable);
    QVERIFY(!compare(missing,token,token));
    QVERIFY(!mint(state,2,Affinity::left)); QVERIFY(!mint(state,6,Affinity::left));
    QVERIFY(!mint(state,0,static_cast<Affinity>(9)));
    auto bad=token;bad.right=bad.left; QCOMPARE(resolve(state,bad).status,Status::invalid_input);
    bad=token;bad.context.document=std::string(257,'x');QCOMPARE(resolve(state,bad).status,Status::invalid_input);
    bad=token;bad.context.document="elsewhere";QCOMPARE(resolve(missing,bad).status,Status::wrong_document);
    bad=token;bad.context.epoch=0;QCOMPARE(resolve(state,bad).status,Status::invalid_input);
    auto inverted=state;std::reverse(inverted.order.begin(),inverted.order.end());
    QCOMPARE(resolve(inverted,token).status,Status::unavailable);
    State list{{"doc","list",Primitive::list,1},{{"A/1",1,true},{"A/3",1,false},{"B/4",1,true}}, {"A/1","A/3","B/4"}};
    QCOMPARE(resolve_element(list,list.context,"B/4"), (Resolution{Status::resolved,1}));
    QCOMPARE(resolve_element(list,list.context,"A/3").status,Status::detached);
    QCOMPARE(resolve_element(list,list.context,"missing").status,Status::unavailable);
    auto foreign=list.context;foreign.stream="other";
    QCOMPARE(resolve_element(list,foreign,"B/4").status,Status::wrong_stream);
    list.order[0].application_value=uint64_t(1)<<63; list.order[2].application_value=20;
    auto element=element_of(list,uint64_t(1)<<63); QVERIFY(element);
    QCOMPARE(resolve_element(list,*element).offset,0u);
    auto delayed=list;delayed.integrated.clear();QCOMPARE(resolve_element(delayed,*element).status,Status::awaiting_state);
    delayed.integrated=list.integrated;delayed.order.clear();QCOMPARE(resolve_element(delayed,*element).status,Status::unavailable);
    QVERIFY(!element_of(list,999));
}
void BoundaryModelTest::deletionUndoRedo() {
    State state{{"doc","text",Primitive::text,1},{{"a",1,true},{"b",1,true},{"c",1,true}}, {"abc"}};
    auto token=*mint(state,1,Affinity::left);
    state.order[1].visible=false; state.integrated.insert("delete-b");
    QCOMPARE(resolve(state,token),(Resolution{Status::resolved,1,false,true}));
    QCOMPARE(resolve_strict(state,token,true).status,Status::detached);
    QCOMPARE(resolve_strict(state,token,false).status,Status::resolved);
    state.order[1].visible=true;state.integrated.insert("undo-delete-b");
    QCOMPARE(resolve_strict(state,token,true),(Resolution{Status::resolved,1}));
    state.order[1].visible=false;state.integrated.insert("redo-delete-b");
    QCOMPARE(resolve_strict(state,token,true).status,Status::detached);
    state.order.erase(state.order.begin()+1);
    QCOMPARE(resolve_strict(state,token,true).status,Status::unavailable);
    State empty{state.context,{}, {}};
    auto left=*mint(empty,0,Affinity::left),right=*mint(empty,0,Affinity::right);
    QCOMPARE(resolve_strict(empty,left,true).status,Status::invalid_input);
    empty.order={{"later",4,true}};empty.integrated={"insert-later"};
    QCOMPARE(resolve(empty,left).offset,0u);QCOMPARE(resolve(empty,right).offset,4u);
    empty.order.clear();empty.history_available=false;
    QCOMPARE(resolve(empty,left).status,Status::unavailable);
    QCOMPARE(resolve(empty,right).status,Status::unavailable);
    QVERIFY(!mint(empty,0,Affinity::left));
}
void BoundaryModelTest::postMergeAndHiddenMint() {
    State state{{"doc","text",Primitive::text,1},{{"a",1,true},{"x",1,true},{"y",1,true},{"b",1,true}}, {"ab","xy"}};
    auto left=*mint(state,2,Affinity::left),right=*mint(state,2,Affinity::right);
    state.order.insert(state.order.begin()+2,{"nested",4,true});state.integrated.insert("nested-edit");
    QCOMPARE(resolve(state,left).offset,2u);QCOMPARE(resolve(state,right).offset,6u);
    state.order[1].visible=false;state.order[3].visible=false;
    auto collapsed=*mint(state,5,Affinity::left); // visible gap after nested, before b; y is retained and hidden
    QCOMPARE(collapsed.left,std::optional<std::string>("nested"));
    QCOMPARE(collapsed.right,std::optional<std::string>("b"));
    state.order[3].visible=true; // an undone deletion revives y inside the fenced gap
    QCOMPARE(resolve(state,collapsed).offset,5u);
    collapsed.affinity=Affinity::right;QCOMPARE(resolve(state,collapsed).offset,6u);
}
void BoundaryModelTest::boundedStates() {
    const auto& cfg=CollabText::Test::seed_config(); std::mt19937_64 rng(cfg.seed);
    uint64_t orders=0,states=0,resultChecks=0; QCryptographicHash digest(QCryptographicHash::Sha256);
    QByteArray trace="boundary-state-enumeration/1\nseed="+QByteArray::number(cfg.seed)+"\n";
    for(auto primitive:{Primitive::text,Primitive::list}) for(int actors=2;actors<=3;++actors) for(int lengthMask=0;lengthMask<(1<<actors);++lengthMask) {
        std::vector<Item> insertions;
        for(int actor=0;actor<actors;++actor) for(int i=0;i<1+((lengthMask>>actor)&1);++i)
            insertions.push_back({std::string(1,char('A'+actor))+"/"+std::to_string(i+1), primitive == Primitive::list ? 1u : uint32_t(1+((actor+i+cfg.seed)%4)),true});
        std::vector<int> permutation(insertions.size()); std::iota(permutation.begin(),permutation.end(),0);
        State seed{{"enumeration",primitive == Primitive::text ? "text" : "list",primitive,1},{{"a",1,true},{"b",1,true}}, {"seed-ab"}};
        const auto left=*mint(seed,1,Affinity::left),right=*mint(seed,1,Affinity::right);
        const auto selected=ElementToken{seed.context,"b",seed.integrated};
        auto attachmentStatus=[&](const State& s) {
            return primitive == Primitive::text ? resolve_strict(s,left,true).status : resolve_element(s,selected).status;
        };
        do {
            bool causal=true;
            for(size_t p=1;p<permutation.size();++p) for(size_t q=0;q<p;++q)
                if(insertions[permutation[q]].id[0]==insertions[permutation[p]].id[0] && permutation[q]>permutation[p]) causal=false;
            if(!causal) continue;
            ++orders; State final=seed;final.order.pop_back();
            std::vector<std::string> arrivals;
            for(auto i:permutation) {final.order.push_back(insertions[i]);arrivals.push_back(insertions[i].id);final.integrated.insert(insertions[i].id);}
            final.order.push_back(seed.order.back());
            auto delayed=final;delayed.integrated.erase("seed-ab");
            QCOMPARE(resolve(delayed,left).status,Status::awaiting_state);
            QCOMPARE(resolve(delayed,right).status,Status::awaiting_state);
            const auto n=final.order.size();
            for(unsigned mask=0;mask<(1u<<n);++mask) {
                uint32_t expectedLower=0,expectedUpper=0;
                for(size_t p=0;p<n;++p) {
                    final.order[p].visible=!(mask&(1u<<p));
                    if(p==0 && final.order[p].visible) expectedLower=1;
                    if(p+1<n && final.order[p].visible) expectedUpper+=final.order[p].width;
                }
                auto x=resolve(final,left),y=resolve(final,right);
                QCOMPARE(compare(final,left,right),std::optional<int>(expectedLower < expectedUpper ? -1 : 0));
                QCOMPARE(x,(Resolution{Status::resolved,expectedLower,bool(mask&1),bool(mask&(1u<<(n-1)))}));
                QCOMPARE(y,(Resolution{Status::resolved,expectedUpper,x.lost_left,x.lost_right}));
                QCOMPARE(attachmentStatus(final),final.order.back().visible ? Status::resolved : Status::detached);
                // Equivalent reconstructed state receives each insertion twice in shuffled order.
                auto replica=final;replica.integrated=seed.integrated;std::shuffle(arrivals.begin(),arrivals.end(),rng);
                for(auto& id:arrivals) {replica.integrated.insert(id);replica.integrated.insert(id);}
                QCOMPARE(resolve(replica,left),x);QCOMPARE(resolve(replica,right),y);
                // Selective deletion undo/redo changes only the selected retained item's visibility.
                auto restored=final; restored.order.back().visible=true;
                QCOMPARE(attachmentStatus(restored),Status::resolved);
                restored.order.back().visible=false;
                QCOMPARE(attachmentStatus(restored),Status::detached);
                QByteArray row=QByteArray::number(int(primitive))+":"+QByteArray::number(orders)+":"+QByteArray::number(mask)+":"+QByteArray::number(x.offset)+":"+QByteArray::number(y.offset)+":";
                for(auto& id:arrivals) row+=QByteArray::fromStdString(id)+",";
                row+="\n";digest.addData(row);trace+=row;++states;resultChecks+=8;
            }
        } while(std::next_permutation(permutation.begin(),permutation.end()));
    }
    QCOMPARE(orders,472ULL); // independent combinatorial sum over the stated length bounds
    qInfo("seed=%llu supplied-orders=%llu visibility-states=%llu result-checks=%llu sha256=%s",
          (unsigned long long)cfg.seed,(unsigned long long)orders,(unsigned long long)states,(unsigned long long)resultChecks,digest.result().toHex().constData());
    if(!cfg.trace_output_dir.empty()) {
        QDir dir(QString::fromStdString(cfg.trace_output_dir)); QVERIFY(dir.mkpath("."));
        QFile file(dir.filePath("boundary-states.trace"));QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(trace),trace.size());
    }
}
void BoundaryModelTest::mutationCounterexamples() {
    State state{{"doc","text",Primitive::text,1},{{"a",1,true},{"b",1,true}}, {"ab"}};
    auto token=*mint(state,1,Affinity::left);state.order.insert(state.order.begin()+1,{"X",1,true});
    QCOMPARE(resolve(state,token).offset,1u);
    QVERIFY(resolve(state,token,{true,false,false}).offset != 1u);
    auto foreign=token;foreign.context.document="foreign";
    QCOMPARE(resolve(state,foreign).status,Status::wrong_document);
    QVERIFY(resolve(state,foreign,{false,true,false}).status != Status::wrong_document);
    auto missing=state;missing.order.erase(missing.order.begin());
    QCOMPARE(resolve(missing,token).status,Status::unavailable);
    QVERIFY(resolve(missing,token,{false,false,true}).status != Status::unavailable);
    qInfo("Three reduced mutation counterexamples rejected: ignored affinity; erased scope; missing identity mapped to end.");
}
QTEST_GUILESS_MAIN(BoundaryModelTest)
#include "tst_boundary_model.moc"
