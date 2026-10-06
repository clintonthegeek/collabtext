// Demonstrates the pre-P05 defects with NDEBUG-style asserts disabled.
#include "crdt/IdList.h"
#include "crdt/Buffer.h"
#include <cstdio>
#include <string>
using namespace CollabText::Crdt;
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "idlist") {
        IdList list(1);
        list.insert_after(Anchor::min(), 100);
        printf("before remove: ids=%zu\n", list.ids().size());
        // Anchor names an entry that does not exist. Old code: assert(idx < size)
        // then entries[idx] -- a one-past-the-end read.
        list.remove_at(Anchor(1, 999, Bias::Left));
        printf("after remove: ids=%zu\n", list.ids().size());
        return 0;
    }
    // Buffer local edit with mismatched range/text cardinality. Old code:
    // assert(ranges.size() == new_text.size()) only, then indexes new_text[i].
    Buffer buf(1);
    buf.apply_local_edit({{0,0}}, {"hello"});
    printf("before: text=%s len=%zu\n", buf.text().c_str(), buf.text().size());
    std::vector<std::string> one_text = {"a"};
    std::vector<std::pair<uint32_t,uint32_t>> two_ranges = {{0,0},{1,2}};
    buf.apply_local_edit(two_ranges, one_text);
    printf("after: text=%s len=%zu\n", buf.text().c_str(), buf.text().size());
    return 0;
}
