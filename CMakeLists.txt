cmake_minimum_required(VERSION 3.20)

project(serpent CXX)

option(SERPENT_TEST_SANITIZE "Build the tests with AddressSanitizer and UndefinedBehaviorSanitizer" ON)

# Reading and writing unaligned values has nothing to do with serialization, so it is its own
# library. Fetched before BUILD_TESTING is set, so it does not build its tests here.
include(FetchContent)
FetchContent_Declare(unaligned
        GIT_REPOSITORY git@github.com:nebkat/cpp-unaligned.git
        GIT_TAG v0.1.0
        GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(unaligned)

if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    set(BUILD_TESTING ON)
endif()

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
