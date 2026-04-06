cmake_minimum_required(VERSION 3.19)
project(collabtext-testapp VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

qt_add_executable(collabtext-testapp main.cpp)
target_link_libraries(collabtext-testapp PRIVATE Qt6::Widgets CollabText::CollabText)
target_include_directories(collabtext-testapp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../libs/collabtext/src)
