cmake_minimum_required(VERSION 3.19)
project(collabtext-lib VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Test)

add_library(collabtext STATIC
    src/crdt/Clock.cpp
    src/crdt/Locator.cpp
)
set_target_properties(collabtext PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(CollabText::CollabText ALIAS collabtext)

target_include_directories(collabtext
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(collabtext PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets)

# Tests (added as sources are created)
enable_testing()

function(add_crdt_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE collabtext Qt6::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_crdt_test(tst_clock)
add_crdt_test(tst_locator)
