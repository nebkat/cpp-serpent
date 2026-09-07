cmake_minimum_required(VERSION 3.20)

project(serpent CXX)

option(SERPENT_TEST_SANITIZE "Build the tests with AddressSanitizer and UndefinedBehaviorSanitizer" ON)

# Enabled before the subdirectory, so its tests register with this project's ctest.
if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    set(BUILD_TESTING ON)
endif()

# Reading and writing unaligned values is useful with no serialization anywhere near it, so
# it is its own library and can be lifted out as a directory move.
add_subdirectory(unaligned)

# Header only.
add_library(serpent INTERFACE)
add_library(serpent::serpent ALIAS serpent)

target_include_directories(serpent INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(serpent INTERFACE unaligned::unaligned)
target_compile_features(serpent INTERFACE cxx_std_23)

if(PROJECT_IS_TOP_LEVEL)
    add_subdirectory(test)
    add_subdirectory(example)
endif()
