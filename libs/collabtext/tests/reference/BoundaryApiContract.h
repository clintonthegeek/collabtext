#pragma once
#include <collabtext/Anchor.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>
#include <type_traits>
namespace CollabText::PositionApiTarget {
using CollabText::Primitive;
using CollabText::Affinity;
using CollabText::ReferentSide;
using CollabText::PositionScope;
using CollabText::TextBoundary;
using CollabText::TextAttachment;
using CollabText::ListBoundary;
using CollabText::ListElement;
using CollabText::ResolvedTextPosition;
using CollabText::ResolvedListPosition;
using CollabText::PositionToken;
using CollabText::CrdtEngine;
using CollabText::IdListEngine;
static_assert(std::is_same_v<decltype(&CrdtEngine::create),
    Result<std::unique_ptr<CrdtEngine>> (*)(Crdt::SharedIdentityAllocator, PositionScope)>);
static_assert(std::is_same_v<decltype(&CrdtEngine::boundary_at),
    Result<TextBoundary> (CrdtEngine::*)(uint32_t, Affinity) const>);
static_assert(std::is_same_v<decltype(&IdListEngine::boundary_at),
    Result<ListBoundary> (IdListEngine::*)(uint32_t, Affinity) const>);
static_assert(std::is_same_v<decltype(&IdListEngine::element_of),
    Result<ListElement> (IdListEngine::*)(uint64_t) const>);
} // namespace CollabText::PositionApiTarget
