cmake_minimum_required(VERSION 3.19)
project(collabtext VERSION 0.1.0 LANGUAGES CXX)

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
option(COLLABTEXT_BUILD_QT             "Build the Qt adapter and widget components" ON)
option(COLLABTEXT_BUILD_APPS           "Build the dogfood applications"        ON)
option(COLLABTEXT_BUILD_FILE_TRANSPORT "Build the file-backed transport component" ON)

# QtTest is only ever needed by tests, and Qt Widgets only by the widget
# component and the apps. Deriving the requirement here rather than in the
# subdirectory is what keeps a tests-off, Qt-off configure from looking for Qt.
set(COLLABTEXT_NEEDS_QT OFF)
if(COLLABTEXT_BUILD_QT OR COLLABTEXT_BUILD_APPS OR COLLABTEXT_BUILD_TESTS)
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
    if(COLLABTEXT_BUILD_TESTS)
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

add_subdirectory(libs/collabtext)

if(COLLABTEXT_BUILD_APPS)
    add_subdirectory(app)
endif()

if(COLLABTEXT_BUILD_TESTS)
    enable_testing()

    # Trace reduction tool (roadmap V04). Reads a versioned trace artifact
    # produced by a COLLABTEXT_TRACE_OUTPUT campaign and emits a smaller trace
    # that still reproduces the same failure. A developer tool, not a CTest test,
    # and it depends on the test harness rather than on the library.
    add_executable(reduce-trace tools/reduce-trace.cpp)
    target_link_libraries(reduce-trace PRIVATE CollabText::TestSupport)
    target_include_directories(reduce-trace PRIVATE ${CMAKE_SOURCE_DIR}/libs/collabtext/src
                                               ${CMAKE_SOURCE_DIR}/libs/collabtext/tests)
endif()