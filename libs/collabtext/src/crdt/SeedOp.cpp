#include "crdt/SeedOp.h"
#include "crdt/Buffer.h"

namespace CollabText::Crdt {

Operation op_for_seed(const std::string& content) {
    Buffer seed_buffer(0);
    return seed_buffer.apply_local_edit({{0, 0}}, {content});
}

} // namespace CollabText::Crdt
