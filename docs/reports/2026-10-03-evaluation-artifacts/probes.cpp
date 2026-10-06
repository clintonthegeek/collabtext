#include "collabtext/StreamSync.h"
#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "crdt/SegmentReader.h"
#include "crdt/SegmentWriter.h"
#include "crdt/Serialization.h"
#include <filesystem>
#include <iostream>
using namespace CollabText::Crdt;
namespace fs = std::filesystem;
int main() {
  {
    Buffer a(1);
    a.apply_local_edit({{0, 0}}, {"ab"});
    auto l = a.anchor_at(1, Bias::Left), r = a.anchor_at(1, Bias::Right);
    a.apply_local_edit({{1, 1}}, {"X"});
    std::cout << "anchor exact insertion text='" << a.text()
              << "' left=" << a.resolve_anchor(l)
              << " right=" << a.resolve_anchor(r) << " expected=1,2\n";
  }
  {
    Global a, b;
    a.observe(Lamport(1, 10));
    a.meet(b);
    std::cout << "watermark meet {1:10} with empty: component1=" << a.get(1)
              << " expected=0\n";
  }
  {
    Buffer a(1), b(2);
    auto i = a.apply_local_edit({{0, 0}}, {"abc"});
    b.apply_ops({i});
    auto u = a.undo();
    b.apply_ops({*u});
    auto n = b.collect_garbage();
    auto r = a.redo();
    b.apply_ops({*r});
    std::cout << "remote insertion undo GC removed=" << n << " text A='"
              << a.text() << "' B='" << b.text() << "'\n";
  }
  {
    Buffer a(1), b(2);
    auto i = a.apply_local_edit({{0, 0}}, {"abc"});
    auto j = b.apply_local_edit({{0, 0}}, {"XYZ"});
    a.apply_ops({j});
    b.apply_ops({i});
    std::cout << "concurrent paste A='" << a.text() << "' B='" << b.text()
              << "'\n";
  }

  {
    Buffer a(1), b(2);
    auto i = a.apply_local_edit({{0, 0}}, {"abc"});
    b.apply_ops({i});
    auto u = a.undo();
    b.apply_ops({*u});
    auto r = a.redo();
    b.apply_ops({*r});
    std::cout << "buffer undo/redo clocks=" << get_op_timestamp(*u).value << ","
              << get_op_timestamp(*r).value << " text A='" << a.text()
              << "' B='" << b.text() << "'\n";
  }
  {
    IdList a(1), b(2);
    auto i = a.insert_after(Anchor::min(), 42);
    b.apply_ops({i});
    auto u = a.undo();
    b.apply_ops({*u});
    auto r = a.redo();
    b.apply_ops({*r});
    std::cout << "idlist undo/redo clocks=" << get_idlist_op_timestamp(*u).value
              << "," << get_idlist_op_timestamp(*r).value
              << " sizes=" << a.size() << "," << b.size() << "\n";
  }
  {
    Buffer a(1), b(2);
    auto i = a.apply_local_edit({{0, 0}}, {"abc"});
    b.apply_ops({i});
    auto d = b.apply_local_edit({{0, 3}}, {""});
    a.apply_ops({d});
    Global w = a.version();
    w.meet(b.version());
    auto n = a.compact(w);
    auto u = b.undo();
    a.apply_ops({*u});
    std::cout << "compact before remote undo removed=" << n << " text A='"
              << a.text() << "' B='" << b.text() << "'\n";
  }
  {
    Buffer a(1);
    a.apply_local_edit({{0, 0}}, {"abc"});
    a.undo();
    auto n = a.collect_garbage();
    a.redo();
    std::cout << "local GC after insertion undo removed=" << n << " redo text='"
              << a.text() << "'\n";
  }
  {
    Buffer a(65535);
    auto i = a.apply_local_edit({{0, 0}}, {"a"});
    auto j = a.apply_local_edit({{1, 1}}, {"b"});
    std::cout << "high replica serialized second op bytes="
              << encode_operation(j).size() << "\n";
  }
  {
    std::cout << "decoder accepts missing fields="
              << bool(decode_operation("{\"t\":\"e\"}")) << " trailing garbage="
              << bool(decode_operation("{\"t\":\"u\"}garbage")) << "\n";
  }
  fs::path root =
      fs::temp_directory_path() /
      ("collabtext-review-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  {
    Buffer a(1), b(2);
    StreamSync receiver(root / "seq", "B", 2);
    receiver.register_stream("doc", StreamSync::StreamType::AppendOnly);
    receiver.start();
    receiver.set_on_inbound(
        [&](auto &, auto, auto &p) { b.apply_ops({*decode_operation(p)}); });
    {
      StreamSync s(root / "seq", "A", 1);
      s.register_stream("doc", StreamSync::StreamType::AppendOnly);
      s.start();
      s.push("doc", encode_operation(a.apply_local_edit({{0, 0}}, {"a"})));
      s.flush();
      receiver.poll();
    }
    {
      StreamSync s(root / "seq", "A", 1);
      s.register_stream("doc", StreamSync::StreamType::AppendOnly);
      s.start();
      s.push("doc", encode_operation(a.apply_local_edit({{1, 1}}, {"b"})));
      s.flush();
      receiver.poll();
    }
    std::cout << "stream sender restart text A='" << a.text() << "' B='"
              << b.text() << "'\n";
  }
  {
    Buffer a(1);
    StreamSync s(root / "replay", "A", 1);
    s.register_stream("doc", StreamSync::StreamType::AppendOnly);
    s.start();
    s.push("doc", encode_operation(a.apply_local_edit({{0, 0}}, {"abc"})));
    s.flush();
    for (int session = 0; session < 2; ++session) {
      Buffer b(2);
      StreamSync r(root / "replay", "B", 2);
      r.register_stream("doc", StreamSync::StreamType::AppendOnly);
      r.start();
      r.set_on_inbound(
          [&](auto &, auto, auto &p) { b.apply_ops({*decode_operation(p)}); });
      auto n = r.poll();
      std::cout << "receiver session " << session << " received=" << n
                << " text='" << b.text() << "'\n";
    }
  }
  {
    auto dir = root / "gap";
    auto cursor = root / "gap.cursor";
    {
      SegmentWriter w(dir, SegmentKind::Ops);
      w.start();
      w.append("first", 1);
      w.close();
    }
    {
      SegmentWriter w(dir, SegmentKind::Ops);
      w.start();
      w.append("second", 2);
      w.close();
    }
    auto p1 = dir / "0000000001.seg.zst";
    auto hold = root / "held.seg.zst";
    fs::rename(p1, hold);
    SegmentReader r(dir, cursor);
    r.start();
    auto v = r.read_new();
    r.commit();
    fs::rename(hold, p1);
    auto later = r.read_new();
    std::cout << "out-of-order segments first poll=" << v.size()
              << " late segment records=" << later.size() << "\n";
  }
  fs::remove_all(root);
}
