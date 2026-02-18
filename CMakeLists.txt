cmake_minimum_required(VERSION 3.16)

project(BlitzView VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt6 configuration
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Use qt_standard_project_setup if available (Qt 6.3+)
if(COMMAND qt_standard_project_setup)
    qt_standard_project_setup()
endif()

add_subdirectory(src)
