#pragma once

// Private payload builders and read-only access. Never installed/exported.
#include <collabtext/Operations.h>
#include <collabtext/IdListOperations.h>
#include "crdt/Placement.h"
#include <string>
#include <variant>
#include <vector>
#include <utility>

namespace CollabText::Crdt {
struct EditOperation {
    operator Operation() const &;
    operator Operation() &&;
    bool marker_edit = false;
    Identity timestamp;       ///< Operation identity of the edit.
    Global version;           ///< Causal dependencies (version vector). Evolution-reserved.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  ///< (start, end) byte offsets in visible text. Evolution-reserved.
    std::vector<std::string> new_text;                   ///< One replacement string per range. Evolution-reserved.

    struct InsertedFragment {
        Identity origin;        ///< Item identity of this fragment's first item.
        /// Where the run attaches in the FugueMax tree: parent, side and right
        /// origin of its FIRST item. Later items of the run are right children of
        /// the one before and are derived, not stored. Evolution-reserved.
        Link link;
        std::string content;
        uint32_t length;
        bool marker = false;
    };
    std::vector<InsertedFragment> inserted_fragments;    ///< Evolution-reserved.

    /// A run of consecutive item identities from one actor, all deleted by the
    /// same operation. One actor per run is a property of the allocation scheme
    /// (docs/specs/vnext-identity.md §3.2): an operation's items are consecutive
    /// positions of its reserved interval, so a fragment's items are consecutive.
    struct DeletionRun {
        Identity first;
        uint32_t count;
    };
    std::vector<DeletionRun> deletion_runs;              ///< Evolution-reserved.

    Identity deletion_id;     ///< Operation identity that deleted. Evolution-reserved.
};

/// An undo/redo operation produced by Buffer.
///
/// All fields are evolution-reserved except the stable contract accessors.
struct UndoOperation {
    operator Operation() const &;
    operator Operation() &&;
    Identity timestamp;
    Global version;                                       ///< Evolution-reserved.
    std::vector<std::pair<Identity, uint32_t>> counts;   ///< Evolution-reserved.
};


struct IdListInsertOp {
    operator IdListOperation() const &;
    operator IdListOperation() &&;
    Identity timestamp;    ///< Origin of the new element (== this operation's identity).
    Global version;        ///< Causal dependencies. Evolution-reserved.
    uint64_t id = 0;       ///< Opaque application-owned element value. Evolution-reserved.
    Link link;             ///< Where the element attaches in the FugueMax tree. Evolution-reserved.
};

/// Remove the element identified by target_origin.
/// The receiver finds the entry by its origin Lamport and tombstones it.
///
/// All fields other than timestamp are evolution-reserved.
struct IdListRemoveOp {
    operator IdListOperation() const &;
    operator IdListOperation() &&;
    Identity timestamp;     ///< Identity of this deletion op (the deletion_id).
    Global version;         ///< Causal dependencies. Evolution-reserved.
    Identity target_origin; ///< Item identity of the entry being removed. Evolution-reserved.
};

/// Undo/redo operation for IdList; mirrors Buffer's UndoOperation exactly.
///
/// All fields other than timestamp are evolution-reserved.
struct IdListUndoOpVariant {
    operator IdListOperation() const &;
    operator IdListOperation() &&;
    Identity timestamp;
    Global version;                                        ///< Evolution-reserved.
    std::vector<std::pair<Identity, uint32_t>> counts;    ///< Evolution-reserved.
};


namespace detail {
struct OperationPayload { std::variant<EditOperation, UndoOperation> value; };
struct IdListOperationPayload {
    std::variant<IdListInsertOp, IdListRemoveOp, IdListUndoOpVariant> value;
};
struct OperationAccess {
    static const auto& payload(const Operation& op) {
        static const OperationPayload empty{};
        return (op.m_payload ? *op.m_payload : empty).value;
    }
    static const auto& payload(const IdListOperation& op) {
        static const IdListOperationPayload empty{};
        return (op.m_payload ? *op.m_payload : empty).value;
    }
    template<class T> static Operation make_text(T&& builder) {
        return Operation(std::make_shared<const OperationPayload>(
            OperationPayload{std::forward<T>(builder)}));
    }
    template<class T> static IdListOperation make_list(T&& builder) {
        return IdListOperation(std::make_shared<const IdListOperationPayload>(
            IdListOperationPayload{std::forward<T>(builder)}));
    }
};
} // namespace detail

template<class Visitor, class Op>
decltype(auto) visit_operation(Visitor&& visitor, const Op& op) {
    return std::visit(std::forward<Visitor>(visitor), detail::OperationAccess::payload(op));
}
template<class T, class Op>
const T& operation_get(const Op& op) {
    return std::get<T>(detail::OperationAccess::payload(op));
}
template<class T, class Op>
const T* operation_get_if(const Op* op) {
    return op ? std::get_if<T>(&detail::OperationAccess::payload(*op)) : nullptr;
}
template<class T, class Op>
bool operation_holds(const Op& op) {
    return std::holds_alternative<T>(detail::OperationAccess::payload(op));
}
inline Identity get_idlist_op_identity(const IdListOperation& op) {
    return idlist_op_identity(op);
}
inline const Global& get_idlist_op_version(const IdListOperation& op) {
    return visit_operation([](const auto& builder) -> const Global& { return builder.version; }, op);
}

inline EditOperation::operator Operation() const & {
    return detail::OperationAccess::make_text(*this);
}
inline EditOperation::operator Operation() && {
    return detail::OperationAccess::make_text(std::move(*this));
}

inline UndoOperation::operator Operation() const & {
    return detail::OperationAccess::make_text(*this);
}
inline UndoOperation::operator Operation() && {
    return detail::OperationAccess::make_text(std::move(*this));
}

inline IdListInsertOp::operator IdListOperation() const & {
    return detail::OperationAccess::make_list(*this);
}
inline IdListInsertOp::operator IdListOperation() && {
    return detail::OperationAccess::make_list(std::move(*this));
}

inline IdListRemoveOp::operator IdListOperation() const & {
    return detail::OperationAccess::make_list(*this);
}
inline IdListRemoveOp::operator IdListOperation() && {
    return detail::OperationAccess::make_list(std::move(*this));
}

inline IdListUndoOpVariant::operator IdListOperation() const & {
    return detail::OperationAccess::make_list(*this);
}
inline IdListUndoOpVariant::operator IdListOperation() && {
    return detail::OperationAccess::make_list(std::move(*this));
}

} // namespace CollabText::Crdt
