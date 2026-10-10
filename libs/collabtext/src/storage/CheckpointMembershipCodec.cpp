#include "storage/CheckpointCodec.h"

#include "core/Sha256.h"

#include <algorithm>

namespace CollabText::Storage {
namespace {

bool valid_hash(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

}  // namespace

Result<void> validate_checkpoint_core_membership(
    const CheckpointShareableCore &core,
    const MembershipDescriptor &descriptor,
    std::string_view checkpoint_artifact_hash) {
  if (!valid_hash(checkpoint_artifact_hash))
    return Error(ErrorCode::malformed,
                 "checkpoint artifact digest is not a lowercase SHA-256 hash",
                 "checkpoint.membership");
  if (auto valid = validate_membership_descriptor(descriptor); !valid)
    return valid.error().with_context("checkpoint.membership");
  if (core.context.epoch != descriptor.epoch ||
      core.context.parent_descriptor_hash != descriptor.parent_hash)
    return Error(ErrorCode::wrong_epoch,
                 "checkpoint context does not bind to membership descriptor",
                 "checkpoint.membership");
  if (checkpoint_artifact_hash != descriptor.base_checkpoint_hash)
    return Error(ErrorCode::corrupt_record,
                 "checkpoint digest differs from membership base checkpoint",
                 "checkpoint.membership");
  if (auto valid = validate_checkpoint_coverage(core.context, core.coverage,
                                                descriptor); !valid)
    return valid.error().with_context("checkpoint.membership");
  if (auto valid = validate_checkpoint_translations(core.translations,
                                                     descriptor); !valid)
    return valid.error().with_context("checkpoint.membership");
  return Result<void>::success();
}

}  // namespace CollabText::Storage
