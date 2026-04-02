cmake_minimum_required(VERSION 3.19)
project(collabtext VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Test)
qt_standard_project_setup()

add_subdirectory(libs/collabtext)

# App temporarily disabled while migrating from yrs to native CRDT engine
# add_subdirectory(app)

enable_testing()
