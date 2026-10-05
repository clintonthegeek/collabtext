#include "crdt/SeedOp.h"
#include "crdt/Buffer.h"
#include "crdt/DerivedActor.h"

#include <string>

namespace CollabText::Crdt {

ActorId seed_actor(const std::string& content) {
    return derived_actor("collabtext/seed-actor/v1", content);
}

Operation op_for_seed(const std::string& content) {
    Buffer seed_buffer(seed_actor(content));
    return seed_buffer.apply_local_edit({{0, 0}}, {content});
}

} // namespace CollabText::Crdt
