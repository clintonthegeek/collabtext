cmake_minimum_required(VERSION 3.19)
project(collabtext-lib VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Test)

add_library(collabtext STATIC
    src/CrdtEngine.cpp
    src/CollabDocument.cpp
    src/SyncManager.cpp
    src/crdt/Clock.cpp
    src/crdt/Locator.cpp
    src/crdt/UndoMap.cpp
    src/crdt/Buffer.cpp
    src/crdt/Serialization.cpp
    src/crdt/FileSync.cpp
    src/crdt/NetworkSim.cpp
    include/collabtext/CollabDocument.h
    include/collabtext/SyncManager.h
)
set_target_properties(collabtext PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(CollabText::CollabText ALIAS collabtext)

target_include_directories(collabtext
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(collabtext PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)

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
add_crdt_test(tst_filesync)
add_crdt_test(tst_realistic)
