cmake_minimum_required(VERSION 3.19)

# Roadmap task E07a. This is the version a consumer actually sees: it is
# substituted into collabtext-config.cmake as COLLABTEXT_VERSION and it is what
# collabtext-config-version.cmake compares a find_package request against.
#
# Two numbers, deliberately. PROJECT_VERSION must stay numeric -- CMake rejects
# a SemVer prerelease in project(VERSION) -- so it is the release line, and
# COLLABTEXT_PRERELEASE carries the prerelease label that the tag and the
# release notes use. The pair must stay equal to the superproject's in the
# top-level CMakeLists.txt; tools/prepare-integration-alpha.sh checks that.
#
# Note the consequence for consumers: collabtext-config-version.cmake reports
# "0.2.0", not "0.2.0-alpha.1", so a find_package request cannot name the
# prerelease. COLLABTEXT_VERSION_STRING carries the full label.
project(collabtext-lib VERSION 0.2.0 LANGUAGES CXX)
set(COLLABTEXT_PRERELEASE "alpha.2")
set(COLLABTEXT_VERSION_STRING "${PROJECT_VERSION}-${COLLABTEXT_PRERELEASE}")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(COLLABTEXT_BUILD_FUZZERS "Build libFuzzer decoder target (Clang only)" OFF)

if(NOT DEFINED COLLABTEXT_BUILD_FILE_TRANSPORT)
    set(COLLABTEXT_BUILD_FILE_TRANSPORT ON)
endif()

# ===========================================================================
# Component inventory (roadmap task A01)
#
#   Core           the engine: Buffer, IdList, the SumTree, undo, identity,
#                  values and codecs. C++20 only. No Qt, no zstd, no hash
#                  provider, no file transport, no session ownership.
#   FileTransport  file sync, segments, compression and durable-store
#                  composition. Depends on Core; Core never depends on it.
#   QtAdapter      Qt-side integration that is not a widget (SyncManager).
#                  Depends on Core (and FileTransport where it drives streams).
#   Widgets        Qt widget layer plus CollabDocument. Depends on the above.
#   TestSupport    NetworkSim, the trace/fault harness and QtTest. Never linked
#                  into a production target or exported by one.
#
# One-way dependencies only. `storage/DurableFile.cpp` is in Core on purpose:
# ReplicaState's private-state ownership needs portable lease and durable-write
# primitives, so they cannot sit in FileTransport without Core linking back
# upwards to reach them.
# ===========================================================================

# ---------------------------------------------------------------------------
# Core
# ---------------------------------------------------------------------------
add_library(collabtext_core STATIC
    src/AnchorPins.cpp
    src/CrdtEngine.cpp
    src/IdListEngine.cpp
    src/core/Sha256.cpp
    src/codec/Utf.cpp
    src/codec/JsonReader.cpp
    src/crdt/Clock.cpp
    src/crdt/UndoMap.cpp
    src/crdt/Buffer.cpp
    src/crdt/IdList.cpp
    src/codec/OperationCodec.cpp
    src/crdt/PositionTraversal.cpp
    src/codec/AnchorCodec.cpp
    src/crdt/ApplyOutcome.cpp
    src/crdt/OperationValues.cpp
    src/crdt/OperationValidation.cpp
    src/crdt/StreamSerialization.cpp
    src/crdt/ChatMessage.cpp
    src/crdt/Comment.cpp
    src/crdt/SeedOp.cpp
    src/crdt/SequenceOrder.cpp
    src/identity/Identity.cpp
    src/identity/IdentityStore.cpp
    src/identity/IdentityProjector.cpp
    src/identity/PresenceManager.cpp
    src/identity/Signing.cpp
    src/identity/ReplicaState.cpp
    src/identity/ReplicaAllocator.cpp
    src/storage/DurableFile.cpp
    src/storage/CheckpointCodec.cpp
    src/storage/CheckpointCoverageCodec.cpp
    src/storage/CheckpointDeferredCodec.cpp
    src/storage/CheckpointEngineCodec.cpp
    src/storage/CheckpointLocalCodec.cpp
    src/storage/CheckpointPinsCodec.cpp
    src/storage/CheckpointTranslationCodec.cpp
    src/storage/LocalHistoryLog.cpp
    src/storage/MembershipStore.cpp
    src/storage/RetentionState.cpp
)
add_library(CollabText::Core ALIAS collabtext_core)
set_target_properties(collabtext_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME Core          # imported as CollabText::Core
)

# include/ is the intended public surface. src/ is still on the *build*
# interface because several public headers genuinely include from it today --
# <collabtext/Operations.h> includes <crdt/Clock.h>, <collabtext/CrdtEngine.h>
# includes <crdt/IdentityAllocator.h>, and so on. Making it private is A02a's
# job (self-contained public headers); until then, removing it would break every
# consumer of the installed tree.
#
# What this task does fix is the *link* interface: nothing in Core's exported
# dependencies is Qt, zstd, a hash provider, a test, or a transport. An
# include-path leak is visible and annoying; a link dependency is a thing a
# consumer has to install.
# src/ is PRIVATE: Core's own sources include crdt/*.h and core/Sha256.h, and
# nothing outside this target should be able to.
target_include_directories(collabtext_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# FileTransport
# ---------------------------------------------------------------------------
if(COLLABTEXT_BUILD_FILE_TRANSPORT)
    find_package(zstd CONFIG QUIET)
    if(zstd_FOUND)
        if(TARGET zstd::libzstd)
            set(COLLABTEXT_ZSTD_TARGET zstd::libzstd)
        elseif(TARGET zstd::libzstd_shared)
            set(COLLABTEXT_ZSTD_TARGET zstd::libzstd_shared)
        elseif(TARGET zstd::libzstd_static)
            set(COLLABTEXT_ZSTD_TARGET zstd::libzstd_static)
        else()
            message(FATAL_ERROR "zstd CMake config found but no usable target")
        endif()
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
        set(COLLABTEXT_ZSTD_TARGET PkgConfig::ZSTD)
    endif()

    add_library(collabtext_filetransport STATIC
        src/crdt/ZstdUtil.cpp
        src/crdt/SegmentFormat.cpp
        src/crdt/SegmentWriter.cpp
        src/crdt/SegmentReader.cpp
        src/crdt/FileSync.cpp
        src/crdt/StreamSync.cpp
        src/DocumentSession.cpp
        src/crdt/SidecarManifest.cpp
    )
    add_library(CollabText::FileTransport ALIAS collabtext_filetransport)
    set_target_properties(collabtext_filetransport PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        EXPORT_NAME FileTransport)
    # zstd is PRIVATE deliberately: a consumer of the transport needs the
    # symbols, but static-library consumers must not have to *find* zstd to
    # satisfy a link interface. A06a exports the package properly.
    target_link_libraries(collabtext_filetransport
        PUBLIC  collabtext_core
        PRIVATE ${COLLABTEXT_ZSTD_TARGET})
    target_compile_definitions(collabtext_filetransport
        PRIVATE COLLABTEXT_ENABLE_DOCUMENT_SESSION_TEST_ACCESS)
    # The transport has no advertised public header yet: StreamSync's header
    # names SegmentWriter/SegmentReader, which reach zstd, so it lives under
    # src/transport and is not exported. A06a owns the exported form.
    target_include_directories(collabtext_filetransport
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

# ---------------------------------------------------------------------------
# Qt components
# ---------------------------------------------------------------------------
if(COLLABTEXT_BUILD_QT)
    add_library(collabtext_qtadapter STATIC
        src/SyncManager.cpp
    )
    add_library(CollabText::QtAdapter ALIAS collabtext_qtadapter)
    set_target_properties(collabtext_qtadapter PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        EXPORT_NAME QtAdapter)
    target_link_libraries(collabtext_qtadapter PUBLIC collabtext_core Qt6::Core Qt6::Gui)
    if(TARGET collabtext_filetransport)
        target_link_libraries(collabtext_qtadapter PUBLIC collabtext_filetransport)
    endif()
    target_include_directories(collabtext_qtadapter
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_library(collabtext_widgets STATIC
        src/CollabDocument.cpp
        src/ui/AvatarWidget.cpp
        src/ui/PresenceIndicator.cpp
        src/ui/IdentityEditor.cpp
        src/ui/IdentitySetupDialog.cpp
        src/ui/IdentityPreferencesPage.cpp
        src/ui/ParticipantListWidget.cpp
        src/ui/ChatPanelWidget.cpp
        src/ui/CommentsPanelWidget.cpp
        src/ui/CursorLabelWidget.cpp
        src/ui/CollabPlainTextEdit.cpp
        src/ui/MultiCursorController.cpp
    )
    add_library(CollabText::Widgets ALIAS collabtext_widgets)
    set_target_properties(collabtext_widgets PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        EXPORT_NAME Widgets)
    target_link_libraries(collabtext_widgets
        PUBLIC collabtext_qtadapter Qt6::Widgets)
    target_include_directories(collabtext_widgets
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

# ---------------------------------------------------------------------------
# Aggregate alias
#
# Kept so existing consumers -- the dogfood apps, tools and every test -- keep
# building unchanged. A02a is what retires it in favour of naming components.
# ---------------------------------------------------------------------------
add_library(collabtext INTERFACE)
add_library(CollabText::CollabText ALIAS collabtext)
target_link_libraries(collabtext INTERFACE collabtext_core)
if(TARGET collabtext_filetransport)
    target_link_libraries(collabtext INTERFACE collabtext_filetransport)
endif()
if(COLLABTEXT_BUILD_QT)
    target_link_libraries(collabtext INTERFACE collabtext_widgets collabtext_qtadapter)
endif()

# ===========================================================================
# Tests and test support
#
# TestSupport is where NetworkSim, the trace harness and QtTest live. It is a
# separate target so that no production target and no exported link interface
# mentions a test dependency: `CollabText::Core`'s interface is Core, full stop.
# ===========================================================================
if(COLLABTEXT_BUILD_TESTS)
    enable_testing()

    if(COLLABTEXT_BUILD_QT)
        add_library(collabtext_public_headers_qt OBJECT
            tests/public-headers/only_CollabDocument_h.cpp
            tests/public-headers/only_PresenceManager_h.cpp
            tests/public-headers/only_SyncManager_h.cpp
        )
        target_link_libraries(collabtext_public_headers_qt PRIVATE collabtext_widgets)
    endif()

endif()

if(COLLABTEXT_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "COLLABTEXT_BUILD_FUZZERS requires Clang libFuzzer")
    endif()
    if(NOT TARGET collabtext_filetransport)
        message(FATAL_ERROR "fuzz_decode requires COLLABTEXT_BUILD_FILE_TRANSPORT=ON")
    endif()
    add_executable(fuzz_decode tests/fuzz_decode.cpp)
    target_link_libraries(fuzz_decode PRIVATE CollabText::Core collabtext_filetransport)
    target_include_directories(fuzz_decode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(fuzz_decode PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(fuzz_decode PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(COLLABTEXT_BUILD_TESTS OR COLLABTEXT_BUILD_BENCHMARKS)
    add_library(collabtext_testsupport STATIC
        src/crdt/NetworkSim.cpp
    )
    # src/ is no longer anyone's public interface, so every in-tree consumer
    # names it explicitly. Tests are the reason it stays reachable at all.
    set(COLLABTEXT_INTERNAL_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_library(CollabText::TestSupport ALIAS collabtext_testsupport)
    set_target_properties(collabtext_testsupport PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(collabtext_testsupport
        PUBLIC collabtext Qt6::Core Qt6::Test)
    # NetworkSim lives under src/crdt/ and is compiled into test support, so the
    # internal include path is PRIVATE here. Test fixtures and harnesses include
    # each other by bare name, so the tests directory is public to tests.
    target_include_directories(collabtext_testsupport
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>
        PRIVATE ${COLLABTEXT_INTERNAL_INCLUDES})

    function(add_crdt_test name)
        add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE CollabText::TestSupport Qt6::Test)
        target_include_directories(${name} PRIVATE ${COLLABTEXT_INTERNAL_INCLUDES}
                                               ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
endif()

if(COLLABTEXT_BUILD_BENCHMARKS)
    add_executable(tst_benchmark EXCLUDE_FROM_ALL tests/tst_benchmark.cpp)
    target_link_libraries(tst_benchmark PRIVATE CollabText::TestSupport Qt6::Test)
    target_include_directories(tst_benchmark PRIVATE ${COLLABTEXT_INTERNAL_INCLUDES})
    # Run explicitly; neither the normal build nor any CTest selection runs it.
endif()

# ===========================================================================
# Core smoke
#
# A plain C++ executable with no QtTest, no Qt and no test support, so that
# "Core builds and links on its own" is an executable fact rather than an
# inspection of target properties. Built whenever Core is; registered with CTest
# only when tests are on.
# ===========================================================================
# Public-header independence (A02a). One object library per half of the
# advertised surface, each TU including exactly one public header and nothing
# else from the library, compiled against the public include tree only. A header
# that reaches into crdt/ or src/ -- or that pulls Qt into Core -- fails here.
add_library(collabtext_public_headers_core OBJECT
    tests/public-headers/only_ActorId_h.cpp
    tests/public-headers/only_Anchor_h.cpp
    tests/public-headers/only_AnchorPins_h.cpp
    tests/public-headers/only_ApplyOutcome_h.cpp
    tests/public-headers/only_CrdtEngine_h.cpp
    tests/public-headers/only_Engine_h.cpp
    tests/public-headers/only_Identity_h.cpp
    tests/public-headers/only_IdentityProjector_h.cpp
    tests/public-headers/only_IdentityStore_h.cpp
    tests/public-headers/only_IdListEngine_h.cpp
    tests/public-headers/only_IdListOperations_h.cpp
    tests/public-headers/only_LocalHistory_h.cpp
    tests/public-headers/only_Operations_h.cpp
    tests/public-headers/only_OpStream_h.cpp
    tests/public-headers/only_Serialization_h.cpp
    tests/public-headers/only_Signing_h.cpp
)
target_link_libraries(collabtext_public_headers_core PRIVATE collabtext_core)

# The alpha consumer: a plain C++ program that edits text and list content and
# moves operations between two peers using only public types and public
# functions. It is the executable form of A02a's acceptance criterion.
add_executable(collabtext_alpha_consumer tests/alpha-consumer/main.cpp)
target_link_libraries(collabtext_alpha_consumer PRIVATE collabtext_core)
if(COLLABTEXT_BUILD_TESTS)
    add_test(NAME collabtext_alpha_consumer COMMAND collabtext_alpha_consumer)
endif()

add_executable(collabtext_core_smoke tests/core-smoke/main.cpp)
target_link_libraries(collabtext_core_smoke PRIVATE collabtext_core)
if(COLLABTEXT_BUILD_TESTS)
    add_test(NAME collabtext_core_smoke COMMAND collabtext_core_smoke)
endif()

# ===========================================================================
# The suites
# ===========================================================================
if(COLLABTEXT_BUILD_TESTS)
add_crdt_test(tst_clock)
add_crdt_test(tst_buffer)
add_crdt_test(tst_convergence)
add_crdt_test(tst_sumtree)
add_crdt_test(tst_anchor)
add_crdt_test(tst_anchor_codec)
add_crdt_test(tst_anchor_retention)
# E07b: the supported-reader fixture for the CTPT/1 token format. Registered as a
# test so a candidate cannot declare a token version supported without it passing.
add_crdt_test(tst_position_token_reader)
add_crdt_test(tst_positions)
add_executable(collabtext_position_replay_worker tests/position-replay/main.cpp)
target_link_libraries(collabtext_position_replay_worker PRIVATE collabtext_core)
add_dependencies(tst_positions collabtext_position_replay_worker)
target_compile_definitions(tst_positions PRIVATE
    COLLABTEXT_POSITION_REPLAY_WORKER="$<TARGET_FILE:collabtext_position_replay_worker>")
add_crdt_test(tst_opqueue)
add_crdt_test(tst_utf8)
add_crdt_test(tst_rope)
add_crdt_test(tst_fuzz)
# S03: production Buffer/IdList against the literal FugueMax reference, link by link.
add_crdt_test(tst_sequence_differential)
add_crdt_test(tst_sequence_fast_paths)
add_crdt_test(tst_trace_reduction)
add_crdt_test(tst_network_sim)
add_crdt_test(tst_store_fault_harness)
add_crdt_test(tst_checkpoint_codec)

# Behaviour fixtures (V08). The fixture directory is compiled in, so the suite
# reads the same files a reviewer reads rather than a copy.
add_crdt_test(tst_behavior_fixtures)
add_crdt_test(tst_actor_identity)
add_crdt_test(tst_summary_laws)
add_crdt_test(tst_decode_validation)
add_crdt_test(tst_operation_validation)
add_crdt_test(tst_operation_values)
# G01's reclamation model is deliberately independent of every production
# target; it uses only values and QtTest, like the S02/S04 model gates.
add_executable(tst_gc_model tests/tst_gc_model.cpp)
target_link_libraries(tst_gc_model PRIVATE Qt6::Test)
target_include_directories(tst_gc_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME tst_gc_model COMMAND tst_gc_model)
# Independent S04a model links QtTest only: no production Core or anchor helpers.
add_executable(tst_boundary_model tests/tst_boundary_model.cpp)
target_link_libraries(tst_boundary_model PRIVATE Qt6::Test)
target_include_directories(tst_boundary_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(tst_boundary_model PRIVATE
    COLLABTEXT_BOUNDARY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_test(NAME tst_boundary_model COMMAND tst_boundary_model)
# S02's independent sequence interpreter. Links QtTest and the two reference
# models only: no production Core, ordering, identity or codec helper, so a
# defect shared with the engine cannot make these pass.
add_executable(tst_sequence_model tests/tst_sequence_model.cpp)
target_link_libraries(tst_sequence_model PRIVATE Qt6::Test)
target_include_directories(tst_sequence_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(tst_sequence_model PRIVATE
    COLLABTEXT_SEQUENCE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sequence-semantics")
add_test(NAME tst_sequence_model COMMAND tst_sequence_model)
# S03 design gate: a literal FugueMax model (arXiv 2305.00583 Algorithm 1 plus the
# FugueMax modifications) that the S02 model and, later, production placement are
# differentially checked against. Links nothing from the engine.
add_executable(tst_fuguemax_model tests/tst_fuguemax_model.cpp)
target_link_libraries(tst_fuguemax_model PRIVATE Qt6::Test)
target_include_directories(tst_fuguemax_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(tst_fuguemax_model PRIVATE
    COLLABTEXT_SEQUENCE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sequence-semantics")
add_test(NAME tst_fuguemax_model COMMAND tst_fuguemax_model)
# S03 drives the PRODUCTION engines through the same S01 fixtures, against the
# same exact-content oracle. It is the counterpart to tst_sequence_model, not a
# variant of it: one runs the reference interpreter, the other runs the engine.
add_crdt_test(tst_sequence_semantics)
target_compile_definitions(tst_sequence_semantics PRIVATE
    COLLABTEXT_SEQUENCE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sequence-semantics")
add_crdt_test(tst_identity_ordering_model)
add_crdt_test(tst_identity_allocation_model)
add_crdt_test(tst_identity_types)
add_crdt_test(tst_identity_allocation_engine)
add_crdt_test(tst_sha256)
add_crdt_test(tst_durable_file)
target_compile_definitions(tst_behavior_fixtures PRIVATE
    COLLABTEXT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/behavior")

# Crash worker for the store fault harness (V07): a separate process that dies
# mid-write on purpose. It is a test dependency, not a test itself — registering
# it with add_test would run a program whose contract is to terminate.
add_executable(store_crash_worker tests/store_crash_worker.cpp)
target_link_libraries(store_crash_worker PRIVATE CollabText::TestSupport Qt6::Test)
target_include_directories(store_crash_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_dependencies(tst_store_fault_harness store_crash_worker)
add_crdt_test(tst_rope_integration)
add_crdt_test(tst_undomap)
add_crdt_test(tst_persistent_undo)
add_crdt_test(tst_membership)
add_crdt_test(tst_undo_retirement)
add_crdt_test(tst_gc)
add_crdt_test(tst_evaluation_regressions)
add_crdt_test(tst_serialization)
add_crdt_test(tst_edits_since)
add_crdt_test(tst_filesync)
add_crdt_test(tst_multicursor)
add_crdt_test(tst_realistic)
add_crdt_test(tst_identity_json)
add_crdt_test(tst_identity_store)
add_crdt_test(tst_identity_projector)
add_crdt_test(tst_presence_manager)
add_crdt_test(tst_identity_widgets)
add_crdt_test(tst_collab_plain_text_edit)
add_crdt_test(tst_scroll_stability)
add_crdt_test(tst_stream_sync)
add_crdt_test(tst_opstream_streamsync)
add_crdt_test(tst_opstream_acks)
add_crdt_test(tst_opstream_convergence_buffer)
add_crdt_test(tst_opstream_convergence_idlist)
add_crdt_test(tst_opstream_convergence_mixed)
add_crdt_test(tst_opstream_replay)
add_crdt_test(tst_session_recovery)
target_compile_definitions(tst_session_recovery
    PRIVATE COLLABTEXT_ENABLE_DOCUMENT_SESSION_TEST_ACCESS)
add_crdt_test(tst_opstream_idempotent)
add_crdt_test(tst_idlist)
add_crdt_test(tst_idlist_anchor)
add_crdt_test(tst_idlist_convergence)
add_crdt_test(tst_idlist_undo)
add_crdt_test(tst_idlist_gc)
add_crdt_test(tst_idlist_fuzz)
add_crdt_test(tst_idlist_serialization)
add_crdt_test(tst_idlist_op_api)
add_crdt_test(tst_chat_message)
add_crdt_test(tst_comment)
add_crdt_test(tst_seed_op)
add_crdt_test(tst_sidecar_manifest)
add_crdt_test(tst_zstd_util)
add_crdt_test(tst_segment_format)
add_crdt_test(tst_segment_writer)
add_executable(segment_writer_crash_worker tests/segment_writer_crash_worker.cpp)
target_link_libraries(segment_writer_crash_worker PRIVATE CollabText::Core collabtext_filetransport)
target_include_directories(segment_writer_crash_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_dependencies(tst_segment_writer segment_writer_crash_worker)
target_compile_definitions(tst_segment_writer PRIVATE
    COLLABTEXT_SEGMENT_WRITER_CRASH_WORKER="$<TARGET_FILE:segment_writer_crash_worker>")
add_crdt_test(tst_segment_reader)

# Public surface test — deliberately excludes the internal src/ include path
# to prove that <collabtext/Serialization.h>, <collabtext/Operations.h>, and
# <collabtext/IdListOperations.h> are self-sufficient.
add_executable(tst_serialization_public tests/tst_serialization_public.cpp)
target_link_libraries(tst_serialization_public PRIVATE CollabText::TestSupport Qt6::Test)
add_test(NAME tst_serialization_public COMMAND tst_serialization_public)

# CrdtEngine public op-API test — excludes internal src/ include path to
# prove that <collabtext/CrdtEngine.h>, <collabtext/Operations.h>, and
# <collabtext/Serialization.h> are self-sufficient.
add_executable(tst_crdtengine_op_api tests/tst_crdtengine_op_api.cpp)
target_link_libraries(tst_crdtengine_op_api PRIVATE CollabText::TestSupport Qt6::Test)
add_test(NAME tst_crdtengine_op_api COMMAND tst_crdtengine_op_api)

# OpStream interface smoke test — excludes internal src/ include path to
# prove that <collabtext/OpStream.h> is self-sufficient with no Qt or internal
# dependencies.
add_executable(tst_opstream_interface tests/tst_opstream_interface.cpp)
target_link_libraries(tst_opstream_interface PRIVATE CollabText::TestSupport Qt6::Test)
add_test(NAME tst_opstream_interface COMMAND tst_opstream_interface)

add_executable(tst_transport_commit_contract tests/tst_transport_commit_contract.cpp)
target_link_libraries(tst_transport_commit_contract PRIVATE CollabText::TestSupport Qt6::Test)
add_test(NAME tst_transport_commit_contract COMMAND tst_transport_commit_contract)

add_crdt_test(tst_store_lifecycle)

endif() # COLLABTEXT_BUILD_TESTS -- the suites

# ===========================================================================
# Install and export (roadmap task A06a)
#
# The point of the exercise is that a consumer must be able to point at an
# install tree with no reference back to this source tree. Three things make
# that true rather than approximately true:
#
#   * `src/` is never installed and never on an exported include path, so there
#     is nothing for a consumer to accidentally depend on.
#   * The package config performs discovery for exactly the components the
#     consumer asked for, and refuses a component the producer did not build with
#     a message naming the option that would enable it.
#   * A consumer can point at the *build* tree as well as an install tree
#     (export(EXPORT ...)), which is what makes the relocation check meaningful:
#     a script can install to prefix A, move it to prefix B, and prove that
#     nothing resolved back to A or to the source.
# ===========================================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(COLLABTEXT_EXPORTED_COMPONENTS core)
if(COLLABTEXT_BUILD_FILE_TRANSPORT)
    list(APPEND COLLABTEXT_EXPORTED_COMPONENTS file-transport)
endif()
if(COLLABTEXT_BUILD_QT)
    list(APPEND COLLABTEXT_EXPORTED_COMPONENTS qt)
endif()
set(COLLABTEXT_EXPORTED_COMPONENTS "${COLLABTEXT_EXPORTED_COMPONENTS}"
    CACHE INTERNAL "components this build exports")

set(_collabtext_export_targets collabtext_core)
if(COLLABTEXT_BUILD_FILE_TRANSPORT)
    list(APPEND _collabtext_export_targets collabtext_filetransport)
endif()
if(COLLABTEXT_BUILD_QT)
    list(APPEND _collabtext_export_targets collabtext_qtadapter collabtext_widgets)
endif()

# Each component exports separately, and the package config includes only the
# files for the components a consumer actually asked for. That is stricter than
# necessary for a shared build -- an unused imported target naming Qt6::Widgets
# would usually be harmless -- and it is the right strictness here: a consumer
# that asked for Core should not have a Qt dependency visible anywhere in its
# configure output, even as a string it never reads.
function(collabtext_install_component name)
    set(targets ${ARGN})
    install(TARGETS ${targets}
        EXPORT CollabText${name}Targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT CollabText${name}Targets
        FILE CollabText${name}Targets.cmake
        NAMESPACE CollabText::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/collabtext)
    export(EXPORT CollabText${name}Targets
        FILE ${CMAKE_CURRENT_BINARY_DIR}/CollabText${name}Targets.cmake
        NAMESPACE CollabText::)
endfunction()

collabtext_install_component(Core collabtext_core)
if(COLLABTEXT_BUILD_FILE_TRANSPORT)
    collabtext_install_component(FileTransport collabtext_filetransport)
endif()
if(COLLABTEXT_BUILD_QT)
    collabtext_install_component(Qt collabtext_qtadapter collabtext_widgets)
endif()

# The capability manifest travels with the headers on purpose: it is the
# contract, and a consumer who installed the library should not have to come
# back to this repository to find out what it cannot do.
install(DIRECTORY include/collabtext
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "ALPHA_CAPABILITIES.md")

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/collabtext-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/collabtext-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/collabtext)

# Compatibility matters here: SameMajorVersion is what lets a consumer keep
# working across a rebuild that did not break the major version.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/collabtext-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/collabtext-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/collabtext-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/collabtext)

export(PACKAGE collabtext)

# Each component's export() above also writes a build-tree copy, so a consumer can
# point at either a build tree or an install tree. That is what makes the
# relocation check meaningful: a script installs to prefix A, moves it to prefix
# B, and proves nothing resolved back to A or to the source.
export(PACKAGE collabtext)
