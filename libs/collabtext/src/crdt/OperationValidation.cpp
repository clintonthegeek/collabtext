/// OperationValidation.cpp — see OperationValidation.h for the contract.

#include "crdt/OperationValidation.h"

#include "codec/Utf.h"

#include <limits>
#include <string>
#include <variant>

namespace CollabText::Crdt {

namespace {

using Limits = ValidationLimits;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// Reject counter arithmetic that would wrap. Every interval in an operation
/// is a half-open range [start, start + count); if that sum does not fit in
/// the counter's width then the interval is not describable, and letting the
/// arithmetic wrap turns a malformed operation into a plausible one.
bool interval_fits(uint64_t start, uint32_t count) {
    // Tested by subtraction: `start + count` may itself overflow. The bound is
    // UINT64_MAX rather than UINT64_MAX - 1 because the interval is half-open,
    // so [start, start + count) ending at UINT64_MAX means the last counter in
    // it is UINT64_MAX - 1, which is issuable. UINT64_MAX itself is the
    // exhaustion sentinel and is never issued (identity spec §3.5).
    return count <= std::numeric_limits<uint64_t>::max() - start;
}

[[nodiscard]] ValidationResult malformed(std::string message) {
    return ValidationResult::reject(
        Error(ErrorCode::malformed, std::move(message)));
}

[[nodiscard]] ValidationResult invalid_identity(std::string message) {
    return ValidationResult::reject(
        Error(ErrorCode::invalid_identity, std::move(message)));
}

[[nodiscard]] ValidationResult counter_exhausted(std::string message) {
    return ValidationResult::reject(
        Error(ErrorCode::counter_exhausted, std::move(message)));
}

/// Describe an identity for an error message.
[[nodiscard]] std::string describe(Identity id) {
    return "actor " + id.actor.to_string() + " counter " +
           std::to_string(id.counter);
}

/// A counter of zero is not an identity here, and neither is an invalid actor.
///
/// `Global::observe` discards counter 0 and `observed()` is trivially satisfied
/// by it, so an operation stamped 0 can neither be recorded in the version
/// vector nor deduplicated. Both engines start their clock at 1 for exactly
/// this reason, so a zero can only come from a malformed or hand-built record.
/// An all-zero ActorId is rejected for the same class of reason: there is no
/// actor 0 (P02), so a record carrying one has no author and cannot be ordered
/// against any real identity without inventing a tie-break.
[[nodiscard]] ValidationResult check_counter(Identity id, const char* what) {
    if (id.counter == 0) {
        return invalid_identity(std::string(what) + " has counter 0, which is "
                                 "not an observable identity");
    }
    if (!id.actor.valid()) {
        return invalid_identity(std::string(what) + " carries an invalid actor "
                                 "id, which is not an identity at all");
    }
    return ValidationResult::valid();
}

/// The first dependency in `required` that `current` has not observed.
///
/// Returning the identity rather than a bare "deferred" is what lets a caller
/// show what it is actually waiting for, and what makes the deferral testable
/// without inspecting engine internals.
[[nodiscard]] std::optional<Identity> first_unmet_dependency(
    const Global& required, const Global& current) {
    for (size_t i = 0; i < required.pair_count(); ++i) {
        const auto& [who, counter] = required.pair(i);
        const Identity dependency{who, counter};
        if (!current.observed(dependency)) return dependency;
    }
    return std::nullopt;
}

/// A locator is a real position only if it lies strictly inside the ordering
/// space. `Locator::min()` and `Locator::max()` are the sentinels an insertion
/// *between* two positions is computed from; storing one as an insertion
/// position would put a character at the boundary of the space rather than in
/// it, and every later insertion would have to order against it.
[[nodiscard]] bool is_usable_locator(const Locator& locator) {
    return locator > Locator::min() && locator < Locator::max();
}

// ---------------------------------------------------------------------------
// Range / text checks
// ---------------------------------------------------------------------------

/// Ranges must be a sorted, non-overlapping cover of the text being replaced.
///
/// The engine's edit path walks ranges left to right against a single forward
/// cursor and assumes each range begins at or after the previous one ended.
/// Overlapping or descending ranges do not merely describe something odd: they
/// make that walk consume the wrong bytes, so the corruption is silent.
[[nodiscard]] ValidationResult check_ranges(
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto [start, end] = ranges[i];
        if (start > end) {
            return malformed("range " + std::to_string(i) + " starts at " +
                             std::to_string(start) + " but ends at " +
                             std::to_string(end));
        }
        if (i > 0) {
            const auto [prev_start, prev_end] = ranges[i - 1];
            if (start < prev_start) {
                return malformed("range " + std::to_string(i) +
                                 " starts at " + std::to_string(start) +
                                 ", before range " + std::to_string(i - 1) +
                                 " at " + std::to_string(prev_start));
            }
            if (start < prev_end) {
                return malformed("range " + std::to_string(i) + " [" +
                                 std::to_string(start) + "," +
                                 std::to_string(end) + ") overlaps range " +
                                 std::to_string(i - 1) + " ending at " +
                                 std::to_string(prev_end));
            }
        }
    }
    return ValidationResult::valid();
}

}  // namespace

// ---------------------------------------------------------------------------
// ValidationResult
// ---------------------------------------------------------------------------

ValidationResult ValidationResult::valid() { return ValidationResult{}; }

ValidationResult ValidationResult::defer(Identity missing) {
    ValidationResult result;
    result.verdict = ValidationVerdict::deferred;
    result.awaiting = missing;
    return result;
}

ValidationResult ValidationResult::reject(Error error) {
    ValidationResult result;
    result.verdict = ValidationVerdict::rejected;
    result.error = std::move(error);
    return result;
}

const ValidationLimits& default_validation_limits() {
    static const ValidationLimits limits{};
    return limits;
}

// ---------------------------------------------------------------------------
// Operation validation
// ---------------------------------------------------------------------------

ValidationResult validate_operation_identity(const Operation& op) {
    return std::visit(
        [](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, EditOperation>) {
                if (auto r = check_counter(concrete.timestamp,
                                           "operation timestamp");
                    r.verdict != ValidationVerdict::valid) {
                    return r;
                }
                return check_counter(concrete.deletion_id, "deletion id");
            } else {
                return check_counter(concrete.timestamp, "undo timestamp");
            }
        },
        op);
}

ValidationResult validate_idlist_operation_identity(const IdListOperation& op) {
    return std::visit(
        [](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, IdListInsertOp>) {
                return check_counter(concrete.timestamp, "operation timestamp");
            } else if constexpr (std::is_same_v<T, IdListRemoveOp>) {
                if (auto r = check_counter(concrete.timestamp,
                                           "operation timestamp");
                    r.verdict != ValidationVerdict::valid) {
                    return r;
                }
                return check_counter(concrete.target_origin, "removal target");
            } else {
                return check_counter(concrete.timestamp, "undo timestamp");
            }
        },
        op);
}

ValidationResult validate_edit_operation(const EditOperation& op,
                                         const Global& current) {
    const Limits& limits = default_validation_limits();

    // ---- Declared sizes -------------------------------------------------
    if (op.ranges.size() > limits.max_ranges ||
        op.inserted_fragments.size() > limits.max_inserted_fragments ||
        op.deletion_runs.size() > limits.max_deletion_runs ||
        op.split_relocations.size() > limits.max_split_relocations) {
        return malformed("operation declares more elements than the format "
                         "permits");
    }

    // ---- Identity -------------------------------------------------------
    if (auto r = check_counter(op.timestamp, "operation timestamp");
        r.verdict != ValidationVerdict::valid) {
        return r;
    }
    if (auto r = check_counter(op.deletion_id, "deletion id");
        r.verdict != ValidationVerdict::valid) {
        return r;
    }

    // ---- Edit ranges and replacement text -------------------------------
    if (op.ranges.size() != op.new_text.size()) {
        return malformed("operation declares " +
                         std::to_string(op.ranges.size()) + " ranges but " +
                         std::to_string(op.new_text.size()) +
                         " replacement strings");
    }
    if (auto r = check_ranges(op.ranges); !r.ok()) return r;

    for (size_t i = 0; i < op.new_text.size(); ++i) {
        const auto error = Codec::validate_utf8(op.new_text[i]);
        if (!error.ok()) {
            return ValidationResult::reject(
                Error(ErrorCode::invalid_unicode,
                      "replacement text " + std::to_string(i) + " is not valid UTF-8: " +
                          Codec::describe(error),
                      describe(op.timestamp)));
        }
    }

    // ---- Inserted fragments ---------------------------------------------
    for (size_t i = 0; i < op.inserted_fragments.size(); ++i) {
        const auto& fragment = op.inserted_fragments[i];
        const std::string where =
            "inserted fragment " + std::to_string(i) + " of " +
            describe(op.timestamp);

        if (auto r = check_counter(fragment.origin, where.c_str());
            r.verdict != ValidationVerdict::valid) {
            return r;
        }

        // The declared character count is what every index, seek, and undo key
        // in the engine is derived from. It has to be the number of Unicode
        // scalars actually present, not the byte count and not a number the
        // sender chose.
        size_t scalars = 0;
        if (const auto error = Codec::utf8_length(fragment.content, scalars);
            !error.ok()) {
            return ValidationResult::reject(
                Error(ErrorCode::invalid_unicode,
                      where + " content is not valid UTF-8: " +
                          Codec::describe(error),
                      describe(op.timestamp)));
        }
        if (scalars != fragment.length) {
            return malformed(where + " declares length " +
                             std::to_string(fragment.length) + " but its " +
                             std::to_string(scalars) +
                             " bytes hold " + std::to_string(scalars) +
                             " Unicode scalars");
        }
        if (fragment.length == 0) {
            return malformed(where +
                             " inserts no scalars, which is not an insertion");
        }

        if (!interval_fits(fragment.origin.counter, fragment.length)) {
            return counter_exhausted(where + " covers counters [" +
                                     std::to_string(fragment.origin.counter) +
                                     "," +
                                     std::to_string(fragment.origin.counter +
                                                    fragment.length) +
                                     "), which leaves the counter width");
        }

        if (!is_usable_locator(fragment.locator)) {
            return malformed(where +
                             " carries an ordering sentinel rather than a "
                             "position inside the document");
        }
    }

    // ---- Deletion runs ---------------------------------------------------
    for (size_t i = 0; i < op.deletion_runs.size(); ++i) {
        const auto& run = op.deletion_runs[i];
        const std::string where = "deletion run " + std::to_string(i) + " of " +
                                  describe(op.timestamp);

        if (run.count == 0) {
            return malformed(where +
                             " deletes nothing, so it carries no information");
        }
        if (auto r = check_counter(run.first, where.c_str());
            r.verdict != ValidationVerdict::valid) {
            return r;
        }
        if (!interval_fits(run.first.counter, run.count)) {
            return counter_exhausted(where + " covers counters [" +
                                     std::to_string(run.first.counter) + "," +
                                     std::to_string(run.first.counter +
                                                    run.count) +
                                     "), which leaves the counter width");
        }
    }

    // ---- Split relocations ------------------------------------------------
    for (size_t i = 0; i < op.split_relocations.size(); ++i) {
        const auto& reloc = op.split_relocations[i];
        const std::string where = "split relocation " + std::to_string(i) +
                                  " of " + describe(op.timestamp);

        if (auto r = check_counter(reloc.fragment_origin, where.c_str());
            r.verdict != ValidationVerdict::valid) {
            return r;
        }
        if (reloc.split_offset > reloc.fragment_length) {
            return malformed(where + " splits at " +
                             std::to_string(reloc.split_offset) +
                             " inside a fragment of " +
                             std::to_string(reloc.fragment_length) +
                             " characters");
        }
        if (!interval_fits(reloc.fragment_origin.counter,
                          reloc.fragment_length)) {
            return counter_exhausted(where + " covers counters [" +
                                     std::to_string(reloc.fragment_origin.counter) +
                                     "," +
                                     std::to_string(
                                         reloc.fragment_origin.counter +
                                         reloc.fragment_length) +
                                     "), which leaves the counter width");
        }
        if (!is_usable_locator(reloc.new_locator)) {
            return malformed(where +
                             " relocates to an ordering sentinel rather than a "
                             "position inside the document");
        }
    }

    // ---- Causality, last -------------------------------------------------
    // Reached only once the operation is known to be applicable in principle,
    // so a malformed record is never parked in the deferral queue.
    if (const auto missing = first_unmet_dependency(op.version, current)) {
        return ValidationResult::defer(*missing);
    }
    return ValidationResult::valid();
}

// ---------------------------------------------------------------------------
// UndoOperation
// ---------------------------------------------------------------------------

ValidationResult validate_undo_operation(const UndoOperation& op,
                                          const Global& current) {
    const Limits& limits = default_validation_limits();

    if (op.counts.size() > limits.max_undo_targets) {
        return malformed("undo operation declares more targets than the format "
                         "permits");
    }

    if (auto r = check_counter(op.timestamp, "undo timestamp");
        r.verdict != ValidationVerdict::valid) {
        return r;
    }

    for (size_t i = 0; i < op.counts.size(); ++i) {
        const auto& [target, count] = op.counts[i];
        const std::string where =
            "undo target " + std::to_string(i) + " of " + describe(op.timestamp);

        if (auto r = check_counter(target, where.c_str());
            r.verdict != ValidationVerdict::valid) {
            return r;
        }
        if (count == 0) {
            return malformed(where +
                             " sets an undo count of zero, which records "
                             "nothing");
        }
        // An undo may only reach backwards. A target at or after the undo's own
        // identity describes an operation that had not happened yet when the
        // undo was minted, so the pair cannot have been produced together.
        if (target >= op.timestamp) {
            return malformed(where + " names " + describe(target) +
                             ", which is not older than the undo itself (" +
                             describe(op.timestamp) + ")");
        }
    }

    if (const auto missing = first_unmet_dependency(op.version, current)) {
        return ValidationResult::defer(*missing);
    }
    return ValidationResult::valid();
}

ValidationResult validate_operation(const Operation& op, const Global& current) {
    return std::visit(
        [&](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, EditOperation>) {
                return validate_edit_operation(concrete, current);
            } else {
                return validate_undo_operation(concrete, current);
            }
        },
        op);
}

// ---------------------------------------------------------------------------
// IdListOperation
// ---------------------------------------------------------------------------

ValidationResult validate_idlist_operation(const IdListOperation& op,
                                            const Global& current) {
    const Limits& limits = default_validation_limits();

    return std::visit(
        [&](const auto& concrete) -> ValidationResult {
            using T = std::decay_t<decltype(concrete)>;

            if constexpr (std::is_same_v<T, IdListUndoOpVariant>) {
                if (concrete.counts.size() > limits.max_undo_targets) {
                    return malformed("undo operation declares more targets than "
                                     "the format permits");
                }
                if (auto r = check_counter(concrete.timestamp, "undo timestamp");
                    r.verdict != ValidationVerdict::valid) {
                    return r;
                }
                for (size_t i = 0; i < concrete.counts.size(); ++i) {
                    const auto& [target, count] = concrete.counts[i];
                    const std::string where =
                        "undo target " + std::to_string(i) + " of " +
                        describe(concrete.timestamp);
                    if (auto r = check_counter(target, where.c_str());
                        r.verdict != ValidationVerdict::valid) {
                        return r;
                    }
                    if (count == 0) {
                        return malformed(where +
                                         " sets an undo count of zero, which "
                                         "records nothing");
                    }
                    if (target >= concrete.timestamp) {
                        return malformed(where + " names " + describe(target) +
                                         ", which is not older than the undo "
                                         "itself (" +
                                         describe(concrete.timestamp) + ")");
                    }
                }
            } else {
                if (auto r = check_counter(concrete.timestamp, "operation timestamp");
                    r.verdict != ValidationVerdict::valid) {
                    return r;
                }
                if constexpr (std::is_same_v<T, IdListInsertOp>) {
                    if (!is_usable_locator(concrete.locator)) {
                        return malformed("insert operation carries an ordering "
                                         "sentinel rather than a position "
                                         "inside the list");
                    }
                } else {
                    if (auto r = check_counter(concrete.target_origin,
                                               "removal target");
                        r.verdict != ValidationVerdict::valid) {
                        return r;
                    }
                }
            }

            if (const auto missing =
                    first_unmet_dependency(get_idlist_op_version(op), current)) {
                return ValidationResult::defer(*missing);
            }
            return ValidationResult::valid();
        },
        op);
}

// ---------------------------------------------------------------------------
// Local edits
// ---------------------------------------------------------------------------

Result<void> validate_local_edit(
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
    const std::vector<std::string>& new_text,
    uint32_t visible_length) {
    if (ranges.size() != new_text.size()) {
        return Error(ErrorCode::malformed,
                     "local edit declares " + std::to_string(ranges.size()) +
                         " ranges but " + std::to_string(new_text.size()) +
                         " replacement strings");
    }

    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto [start, end] = ranges[i];
        if (start > end) {
            return Error(ErrorCode::malformed,
                         "local range " + std::to_string(i) + " starts at " +
                             std::to_string(start) + " but ends at " +
                             std::to_string(end));
        }
        // One past the last byte is the legal insertion point at the end, so
        // the bound is inclusive.
        if (end > visible_length) {
            return Error(ErrorCode::malformed,
                         "local range " + std::to_string(i) + " ends at " +
                             std::to_string(end) +
                             ", past the visible length of " +
                             std::to_string(visible_length) + " bytes");
        }
        if (i > 0) {
            const auto [prev_start, prev_end] = ranges[i - 1];
            if (start < prev_start) {
                return Error(ErrorCode::malformed,
                             "local range " + std::to_string(i) +
                                 " starts before range " +
                                 std::to_string(i - 1));
            }
            if (start < prev_end) {
                return Error(ErrorCode::malformed,
                             "local range " + std::to_string(i) +
                                 " overlaps range " + std::to_string(i - 1));
            }
        }
    }

    for (size_t i = 0; i < new_text.size(); ++i) {
        if (const auto error = Codec::validate_utf8(new_text[i]); !error.ok()) {
            return Error(ErrorCode::invalid_unicode,
                         "local replacement " + std::to_string(i) +
                             " is not valid UTF-8: " + Codec::describe(error));
        }
    }

    return Result<void>::success();
}

// ---------------------------------------------------------------------------
// Deferred budget
// ---------------------------------------------------------------------------

namespace {

size_t identity_bytes(Identity id) {
    // The actor is a fixed 16 bytes whether or not it is valid; the counter is
    // only carried when it is non-zero, matching what the operation holds.
    return sizeof(Identity) + (id.counter ? 1 : 0);
}

size_t locator_bytes(const Locator& locator) {
    return sizeof(Locator) + locator.depth() * sizeof(uint64_t);
}

}  // namespace

size_t estimate_operation_bytes(const Operation& op) {
    size_t bytes = sizeof(Identity) * 2;  // timestamp + deletion_id
    return std::visit(
        [&](const auto& concrete) {
            size_t total = bytes;
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, EditOperation>) {
                total += concrete.ranges.size() * sizeof(std::pair<uint32_t, uint32_t>);
                for (const auto& text : concrete.new_text) total += text.size();
                for (const auto& fragment : concrete.inserted_fragments) {
                    total += sizeof(Identity) + locator_bytes(fragment.locator) +
                             fragment.content.size() + sizeof(uint32_t);
                }
                total += concrete.deletion_runs.size() * sizeof(uint32_t) * 2;
                for (const auto& reloc : concrete.split_relocations) {
                    total += sizeof(Identity) * 2 + locator_bytes(reloc.new_locator) +
                             sizeof(uint32_t) * 2;
                }
                total += concrete.version.pair_count() * sizeof(uint64_t);
            } else {
                for (const auto& count : concrete.counts) {
                    total += sizeof(Identity) + sizeof(uint32_t);
                }
                total += concrete.version.pair_count() * sizeof(uint64_t);
            }
            return total;
        },
        op);
}

size_t estimate_operation_bytes(const IdListOperation& op) {
    size_t total = sizeof(Identity);
    total += get_idlist_op_version(op).pair_count() * sizeof(uint64_t);
    return std::visit(
        [&](const auto& concrete) {
            size_t inner = total;
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, IdListInsertOp>) {
                inner += sizeof(uint64_t) + locator_bytes(concrete.locator);
            } else if constexpr (std::is_same_v<T, IdListRemoveOp>) {
                inner += sizeof(Identity);
            } else {
                inner += concrete.counts.size() * (sizeof(Identity) + sizeof(uint32_t));
            }
            return inner;
        },
        op);
}

bool deferred_budget_exhausted(const DeferredBudget& budget,
                                size_t queued_operations,
                                size_t queued_bytes,
                                size_t incoming_bytes) {
    if (budget.max_operations == 0) return true;
    if (queued_operations >= budget.max_operations) return true;
    if (budget.max_bytes == 0) return true;
    // Compare by subtraction so a large sum cannot wrap the check itself.
    if (incoming_bytes > budget.max_bytes) return true;
    return queued_bytes > budget.max_bytes - incoming_bytes;
}

}  // namespace CollabText::Crdt
