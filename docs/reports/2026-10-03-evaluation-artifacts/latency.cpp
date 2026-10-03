#include "collabtext/CrdtEngine.h"
#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "crdt/Serialization.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
using namespace CollabText::Crdt;
using Timer = std::chrono::steady_clock;
template <class F> void measure(const char *label, int n, F f) {
  std::vector<double> v;
  for (int i = 0; i < n; ++i) {
    auto t = Timer::now();
    f(i);
    v.push_back(
        std::chrono::duration<double, std::micro>(Timer::now() - t).count());
  }
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (auto x : v)
    sum += x;
  std::cout << label << " n=" << n << " mean_us=" << sum / n
            << " p50_us=" << v[n / 2] << " p95_us=" << v[n * 95 / 100]
            << " p99_us=" << v[n * 99 / 100] << " max_us=" << v.back() << "\n";
}
int main() {
  std::cout << "sizeof Fragment=" << sizeof(Fragment)
            << " summary=" << sizeof(FragmentSummary)
            << " tree node=" << sizeof(FragmentTree::Node)
            << " Global=" << sizeof(Global) << "\n";
  for (auto size : {10000u, 100000u}) {
    Buffer a(1);
    for (unsigned x = 0; x < size; x += 100)
      a.apply_local_edit({{x, x}}, {std::string(100, 'a')});
    std::mt19937 rng(42);
    std::string label = "Buffer random insert size=" + std::to_string(size);
    measure(label.c_str(), 500, [&](int) {
      auto p = rng() % (a.visible_length() + 1);
      a.apply_local_edit({{p, p}}, {"x"});
    });
    std::cout << "fragments=" << a.fragment_count() << "\n";
  }
  {
    Buffer a(1);
    a.apply_local_edit({{0, 0}}, {std::string(100000, 'a')});
    measure("Buffer append seed 100K", 1000, [&](int) {
      auto p = a.visible_length();
      a.apply_local_edit({{p, p}}, {"x"});
    });
  }
  {
    CollabText::CrdtEngine a(1);
    a.insert(0, std::string(100000, 'a'));
    measure("CrdtEngine append seed 100K", 1000,
            [&](int i) { a.insert(100000 + i, "x"); });
  }
  {
    IdList a(1);
    measure("IdList insert head", 1000,
            [&](int i) { a.insert_after(Anchor::min(), i); });
  }
  for (auto rid : {1, 250, 65535}) {
    Buffer a(rid);
    a.apply_local_edit({{0, 0}}, {"a"});
    auto op = a.apply_local_edit({{1, 1}}, {"b"});
    auto bytes = encode_operation(op).size();
    std::string label = "encode rid=" + std::to_string(rid);
    measure(label.c_str(), 100, [&](int) {
      auto s = encode_operation(op);
      if (s.empty())
        std::abort();
    });
    std::cout << "wire_bytes=" << bytes << "\n";
  }
}
