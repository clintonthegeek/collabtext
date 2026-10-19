cmake_minimum_required(VERSION 3.19)

# Roadmap task E07a. 0.1.0 was the pre-vNext baseline. The alpha candidate is
# 0.2.0-alpha.1: the release line is 0.2.0 and the SemVer prerelease label is
# carried separately, because CMake's project(VERSION) accepts only a plain
# numeric version and rejects "-alpha.1" outright.
#
# The reliability workstream is *named* "v2" internally. That name is a
# workstream label, not a promise of package major version 2, and 2.0.0 here
# would read as an API break nobody has evaluated. The break that did happen --
# Locator removed, CTBO/CTLO at version 0002 -- is already a hard cut inside 0.x.
#
# COLLABTEXT_PRERELEASE must equal the one in libs/collabtext/CMakeLists.txt.
# tools/prepare-integration-alpha.sh refuses a candidate when they disagree.
project(collabtext VERSION 0.2.0 LANGUAGES CXX)
set(COLLABTEXT_PRERELEASE "alpha.5")
set(COLLABTEXT_VERSION_STRING "${PROJECT_VERSION}-${COLLABTEXT_PRERELEASE}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Component options
#
# Roadmap task A01. These exist so a consumer can ask for the engine without the
# file transport, without Qt, or without the apps, and so a Core-only configure
# performs *no package discovery at all* for Qt, zstd or a crypto provider. That
# last part is the acceptance criterion, not a nicety: a Core build that calls
# find_package(Qt6) cannot be used by a consumer who has no Qt.
# ---------------------------------------------------------------------------
option(COLLABTEXT_BUILD_TESTS          "Build the QtTest suites and test support" ON)
option(COLLABTEXT_BUILD_BENCHMARKS     "Build standalone benchmarks (never registered with CTest)" OFF)
option(COLLABTEXT_BUILD_QT             "Build the Qt adapter and widget components" ON)
option(COLLABTEXT_BUILD_APPS           "Build the dogfood applications"        ON)
option(COLLABTEXT_BUILD_FILE_TRANSPORT "Build the file-backed transport component" ON)
option(COLLABTEXT_ENABLE_PERF_DIAGNOSTICS
    "Enable internal per-thread performance counters for profiling builds" OFF)

if(COLLABTEXT_ENABLE_PERF_DIAGNOSTICS)
    add_compile_definitions(COLLABTEXT_ENABLE_PERF_DIAGNOSTICS=1)
endif()

# QtTest is only ever needed by tests, and Qt Widgets only by the widget
# component and the apps. Deriving the requirement here rather than in the
# subdirectory is what keeps a tests-off, Qt-off configure from looking for Qt.
set(COLLABTEXT_NEEDS_QT OFF)
if(COLLABTEXT_BUILD_QT OR COLLABTEXT_BUILD_APPS OR COLLABTEXT_BUILD_TESTS OR COLLABTEXT_BUILD_BENCHMARKS)
    set(COLLABTEXT_NEEDS_QT ON)
endif()

option(COLLABTEXT_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(COLLABTEXT_ENABLE_SANITIZERS)
    if(WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "COLLABTEXT_ENABLE_SANITIZERS requires GCC or Clang on a non-Windows platform")
    endif()
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_cxx_source_compiles("int main() { return 0; }" COLLABTEXT_SANITIZERS_SUPPORTED)
    cmake_pop_check_state()
    if(NOT COLLABTEXT_SANITIZERS_SUPPORTED)
        message(FATAL_ERROR "The selected compiler cannot compile/link ASan and UBSan")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(COLLABTEXT_NEEDS_QT)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui)
    qt_standard_project_setup()
    set(CMAKE_AUTOMOC ON)
    if(COLLABTEXT_BUILD_QT)
        find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
    endif()
    if(COLLABTEXT_BUILD_TESTS OR COLLABTEXT_BUILD_BENCHMARKS)
        find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    endif()
else()
    message(STATUS "collabtext: Qt disabled; Core only")
endif()

if(COLLABTEXT_BUILD_APPS AND NOT COLLABTEXT_BUILD_QT)
    message(FATAL_ERROR
        "COLLABTEXT_BUILD_APPS requires COLLABTEXT_BUILD_QT: the dogfood apps "
        "are Qt widget applications.")
endif()

if(COLLABTEXT_BUILD_TESTS)
    # Enable test registration before configuring sibling app targets so
    # offscreen integration tests declared by the dogfood applications appear
    # in the root CTest manifest too.
    enable_testing()
endif()

add_subdirectory(libs/collabtext)

if(COLLABTEXT_BUILD_APPS)
    add_subdirectory(app)
endif()

if(COLLABTEXT_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(tst_verification_tools)
    add_test(NAME tst_verification_tools
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_verify_changed.py)
    add_custom_target(tst_sequence_fixtures)
    add_test(NAME tst_sequence_fixtures
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_sequence_fixtures.py)

    # Trace reduction tool (roadmap V04). Reads a versioned trace artifact
    # produced by a COLLABTEXT_TRACE_OUTPUT campaign and emits a smaller trace
    # that still reproduces the same failure. A developer tool, not a CTest test,
    # and it depends on the test harness rather than on the library.
    add_executable(reduce-trace tools/reduce-trace.cpp)
    target_link_libraries(reduce-trace PRIVATE CollabText::TestSupport)
    target_include_directories(reduce-trace PRIVATE ${CMAKE_SOURCE_DIR}/libs/collabtext/src
                                               ${CMAKE_SOURCE_DIR}/libs/collabtext/tests)
endif()

# Build a selected set in one dependency traversal. Passing many independent
# targets to Make repeats the shared library/automoc traversal for every target.
# Verification scripts set this list; it does not change compiler flags or APIs.
set(COLLABTEXT_VERIFY_TARGETS "" CACHE STRING "Targets for the verification aggregate")
if(COLLABTEXT_VERIFY_TARGETS)
    foreach(_verify_target IN LISTS COLLABTEXT_VERIFY_TARGETS)
        if(NOT TARGET ${_verify_target})
            message(FATAL_ERROR "Unknown verification target: ${_verify_target}")
        endif()
    endforeach()
    add_custom_target(collabtext_verify_selected DEPENDS ${COLLABTEXT_VERIFY_TARGETS})
endif()
