cmake_minimum_required(VERSION 3.19)
project(collabtext-lib VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Test)

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

add_library(collabtext STATIC
    src/CrdtEngine.cpp
    src/CollabDocument.cpp
    src/SyncManager.cpp
    src/crdt/Clock.cpp
    src/crdt/Locator.cpp
    src/crdt/UndoMap.cpp
    src/crdt/Buffer.cpp
    src/crdt/Serialization.cpp
    src/crdt/ZstdUtil.cpp
    src/crdt/SegmentFormat.cpp
    src/crdt/SegmentWriter.cpp
    src/crdt/SegmentReader.cpp
    src/crdt/FileSync.cpp
    src/crdt/IdList.cpp
    src/crdt/StreamSerialization.cpp
    src/crdt/StreamSync.cpp
    src/crdt/ChatMessage.cpp
    src/crdt/Comment.cpp
    src/crdt/NetworkSim.cpp
    src/crdt/SeedOp.cpp
    src/crdt/SidecarManifest.cpp
    src/ui/MultiCursorController.cpp
    src/ui/CollabPlainTextEdit.cpp
    src/ui/AvatarWidget.cpp
    src/ui/PresenceIndicator.cpp
    src/ui/IdentityEditor.cpp
    src/ui/IdentitySetupDialog.cpp
    src/ui/IdentityPreferencesPage.cpp
    src/ui/ParticipantListWidget.cpp
    src/ui/ChatPanelWidget.cpp
    src/ui/CommentsPanelWidget.cpp
    src/ui/CursorLabelWidget.cpp
    src/identity/Identity.cpp
    src/identity/PresenceManager.cpp
    src/identity/IdentityStore.cpp
    src/identity/IdentityProjector.cpp
    src/identity/Signing.cpp
    include/collabtext/CollabDocument.h
    include/collabtext/SyncManager.h
    include/collabtext/Identity.h
    include/collabtext/Operations.h
    include/collabtext/IdListOperations.h
    include/collabtext/Serialization.h
    include/collabtext/OpStream.h
    include/collabtext/StreamSync.h
)
set_target_properties(collabtext PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(CollabText::CollabText ALIAS collabtext)

target_include_directories(collabtext
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(collabtext PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_link_libraries(collabtext PRIVATE ${COLLABTEXT_ZSTD_TARGET})

# Tests
enable_testing()

function(add_crdt_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE collabtext Qt6::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_crdt_test(tst_clock)
add_crdt_test(tst_locator)
add_crdt_test(tst_buffer)
add_crdt_test(tst_convergence)
add_crdt_test(tst_sumtree)
add_crdt_test(tst_anchor)
add_crdt_test(tst_opqueue)
add_crdt_test(tst_utf8)
add_crdt_test(tst_rope)
add_crdt_test(tst_fuzz)
add_crdt_test(tst_rope_integration)
add_crdt_test(tst_undomap)
add_crdt_test(tst_benchmark)
add_crdt_test(tst_gc)
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
add_crdt_test(tst_segment_reader)

# Public surface test — deliberately excludes the internal src/ include path
# to prove that <collabtext/Serialization.h>, <collabtext/Operations.h>, and
# <collabtext/IdListOperations.h> are self-sufficient.
add_executable(tst_serialization_public tests/tst_serialization_public.cpp)
target_link_libraries(tst_serialization_public PRIVATE collabtext Qt6::Test)
add_test(NAME tst_serialization_public COMMAND tst_serialization_public)

# CrdtEngine public op-API test — excludes internal src/ include path to
# prove that <collabtext/CrdtEngine.h>, <collabtext/Operations.h>, and
# <collabtext/Serialization.h> are self-sufficient.
add_executable(tst_crdtengine_op_api tests/tst_crdtengine_op_api.cpp)
target_link_libraries(tst_crdtengine_op_api PRIVATE collabtext Qt6::Test)
add_test(NAME tst_crdtengine_op_api COMMAND tst_crdtengine_op_api)

# OpStream interface smoke test — excludes internal src/ include path to
# prove that <collabtext/OpStream.h> is self-sufficient with no Qt or internal
# dependencies.
add_executable(tst_opstream_interface tests/tst_opstream_interface.cpp)
target_link_libraries(tst_opstream_interface PRIVATE collabtext Qt6::Test)
add_test(NAME tst_opstream_interface COMMAND tst_opstream_interface)
