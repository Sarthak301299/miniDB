cmake_minimum_required(VERSION 3.12)
project(miniDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src)
add_subdirectory(include)

install(TARGETS miniDB LIBRARY DESTINATION lib)