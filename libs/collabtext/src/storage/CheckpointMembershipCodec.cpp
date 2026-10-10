#include "storage/CheckpointCodec.h"

#include "core/Sha256.h"

namespace CollabText::Storage {

Result<void> validate_checkpoint_core_membership(
    const CheckpointShareableCore &core,
    const MembershipDescriptor &descriptor,
    std::string_view checkpoint_artifact_bytes) {
  if (auto valid = validate_membership_descriptor(descriptor); !valid)
    return valid.error().with_context("checkpoint.membership");
  if (core.context.epoch != descriptor.epoch ||
      core.context.parent_descriptor_hash != descriptor.parent_hash)
    return Error(ErrorCode::wrong_epoch,
                 "checkpoint context does not bind to membership descriptor",
                 "checkpoint.membership");
  if (sha256_hex(checkpoint_artifact_bytes) != descriptor.base_checkpoint_hash)
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
