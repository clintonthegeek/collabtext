#include <QtTest>
#include "CodecTest.h"
#include "TestActor.h"
#include "crdt/Buffer.h"
#include "crdt/OperationPayload.h"
#include "storage/CheckpointCodec.h"
#include <collabtext/Serialization.h>
#include "reference/FugueMaxModel.h"
#include <random>
using namespace CollabText;
using namespace CollabText::Crdt;
namespace FR=FugueReference;
namespace {
FR::Id id(Identity value){return {value.actor.to_string(),value.counter};}
bool less(const FR::Id&a,const FR::Id&b){return std::tie(a.counter,a.actor)<std::tie(b.counter,b.actor);}
void deliver(FR::Tree& model,const EditOperation& operation) {
 for(const auto& f:operation.inserted_fragments) {
  const auto scalars=QString::fromStdString(f.content).toUcs4();
  for(uint32_t k=0;k<f.length;++k) {
   FR::NodeMsg msg;msg.id=id({f.origin.actor,f.origin.counter+k});
   msg.value=f.marker ? "" : QString::fromUcs4(std::u32string(1,static_cast<char32_t>(scalars[k]))).toStdString();
   const Link link=k ? Link{Identity(f.origin.actor,f.origin.counter+k-1),Side::Right,f.link.right_origin} : f.link;
   msg.parent_is_root=!link.parent;if(link.parent)msg.parent=id(*link.parent);
   msg.side=link.side==Side::Left ? FR::Side::Left : FR::Side::Right;
   msg.ro_is_end=!link.right_origin;if(link.right_origin)msg.right_origin=id(*link.right_origin);
   model.deliver(msg);
  }
 }
 for(const auto& run:operation.deletion_runs)
  for(uint32_t k=0;k<run.count;++k)model.remove(id({run.first.actor,run.first.counter+k}));
}
std::vector<FR::Id> order(const Buffer& buffer) {
 std::vector<FR::Id> result;for(const auto&f:buffer.fragments())for(uint32_t k=0;k<f.length;++k)result.push_back(id(f.item_at(k)));return result;
}
}
class TextMarkersTest : public QObject {
 Q_OBJECT
private slots:
 void differential_zero_width_histories() {
  std::mt19937 random(0x4d41524b);
  for(int trial=0;trial<100;++trial) {
   Buffer origin(Test::test_counter(1)),peer(Test::test_counter(2));FR::Tree model(less);
   auto initial=checked_codec(origin.try_apply_local_edit({{0,0}},{"ab😀é"}));
   auto marker=checked_codec(origin.try_insert_marker(2));
   std::vector<Operation> shared{initial,marker};
   const auto target=operation_get<EditOperation>(marker).inserted_fragments[0].origin;
   for(auto&op:shared){deliver(model,operation_get<EditOperation>(op));QCOMPARE(peer.apply_ops({op})[0].kind(),ApplyOutcomeKind::applied);}
   for(int step=0;step<20;++step) {
    auto& author=(random()%2) ? origin : peer;
    const auto side=(random()%2) ? MarkerSide::before : MarkerSide::after;
    const std::string value=(random()%2) ? "😀é" : "x";
    auto operation=checked_codec(author.try_insert_relative(target,side,value,false));
    deliver(model,operation_get<EditOperation>(operation));
    auto& other=(&author==&origin) ? peer : origin;
    QCOMPARE(other.apply_ops({checked_codec(decode_operation(checked_codec(encode_operation(operation))))})[0].kind(),ApplyOutcomeKind::applied);
    QCOMPARE(order(origin),model.traverse_all());QCOMPARE(order(peer),model.traverse_all());
   }
   auto remove=checked_codec(origin.try_apply_local_edit({{0,origin.visible_length()}},{""}));
   deliver(model,operation_get<EditOperation>(remove));
   QCOMPARE(peer.apply_ops({remove})[0].kind(),ApplyOutcomeKind::applied);
   QVERIFY(origin.text().empty());QCOMPARE(checked_codec(origin.marker_offset(target)),0u);
   QCOMPARE(model.visible_at(0),std::optional<FR::Id>(id(target)));
   QCOMPARE(order(origin),model.traverse_all());
  }
 }
 void reject_type_confusion_atomically() {
  Buffer origin(Test::test_counter(1)),receiver(Test::test_counter(2));
  auto initial=checked_codec(origin.try_apply_local_edit({{0,0}},{"ab"}));
  auto marker=checked_codec(origin.try_insert_marker(1));
  receiver.apply_ops({initial,marker});
  auto bad=operation_get<EditOperation>(checked_codec(origin.try_remove_marker(operation_get<EditOperation>(marker).inserted_fragments[0].origin)));
  bad.deletion_runs[0].first=operation_get<EditOperation>(initial).inserted_fragments[0].origin;
  const auto text=receiver.text();const auto version=receiver.version();
  QCOMPARE(receiver.apply_ops({Operation(bad)})[0].kind(),ApplyOutcomeKind::rejected);
  QCOMPARE(receiver.text(),text);QCOMPARE(receiver.version(),version);
  bad.marker_edit=false;
  bad.deletion_runs[0].first=operation_get<EditOperation>(marker).inserted_fragments[0].origin;
  QCOMPARE(receiver.apply_ops({Operation(bad)})[0].kind(),ApplyOutcomeKind::rejected);
  QCOMPARE(receiver.text(),text);QCOMPARE(receiver.version(),version);
  auto malformed=operation_get<EditOperation>(marker);malformed.inserted_fragments[0].content="x";
  QVERIFY(!encode_operation(Operation(malformed)));
  auto bytes=checked_codec(encode_operation(marker));
  for(size_t n=0;n<bytes.size();++n)QVERIFY(!decode_operation(std::string_view(bytes).substr(0,n)));
  QVERIFY(!decode_operation(bytes+"x"));
 }
 void checkpoint_retains_marker_type_and_identity() {
  using namespace Storage;
  auto allocator=Test::test_counter(1);Buffer origin(allocator);
  auto initial=checked_codec(origin.try_apply_local_edit({{0,0}},{"😀é"}));
  auto marker=checked_codec(origin.try_insert_marker(4));
  const auto target=operation_get<EditOperation>(marker).inserted_fragments[0].origin;
  auto snapshot=origin.checkpoint_state();
  CheckpointEngineState state;state.kind=CheckpointEngineKind::Text;state.fragments=snapshot.fragments;
  auto section=checked_codec(encode_checkpoint_engine(state));QCOMPARE(section.version,uint16_t(3));
  auto decoded=checked_codec(decode_checkpoint_engine(section));snapshot.fragments=decoded.fragments;
  Buffer restored(allocator);QVERIFY(restored.restore_checkpoint_state(snapshot));
  QCOMPARE(restored.text(),origin.text());QCOMPARE(checked_codec(restored.marker_offset(target)),4u);
  auto removed=checked_codec(restored.try_remove_marker(target));
  QCOMPARE(origin.apply_ops({removed})[0].kind(),ApplyOutcomeKind::applied);
  QCOMPARE(origin.marker_offset(target).error().code(),ErrorCode::detached);
  section.bytes[6]=2;QVERIFY(!decode_checkpoint_engine(section));
 }
 void scalar_boundaries_and_refusal_do_not_allocate() {
  auto allocator=Test::test_counter(1);Buffer origin(allocator);
  auto initial=checked_codec(origin.try_apply_local_edit({{0,0}},{"😀é"}));
  const auto next=allocator->peek();
  for(uint32_t offset:{1u,2u,3u,5u,7u})QVERIFY(!origin.try_insert_marker(offset));
  QCOMPARE(allocator->peek(),next);QCOMPARE(origin.text(),std::string("😀é"));
  QVERIFY(origin.set_max_undo_bytes(0));
  QVERIFY(!origin.try_insert_marker(4));QCOMPARE(allocator->peek(),next);
 }
};
QTEST_GUILESS_MAIN(TextMarkersTest)
#include "tst_text_markers.moc"
