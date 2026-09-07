cmake_minimum_required(VERSION 3.20)

project(unaligned CXX)

# Reading and writing values that are not aligned for their type, without ever forming a
# pointer or reference to an object that is not there. Independent of anything else here.
add_library(unaligned INTERFACE)
add_library(unaligned::unaligned ALIAS unaligned)

target_include_directories(unaligned INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_features(unaligned INTERFACE cxx_std_23)

if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    set(BUILD_TESTING ON)
endif()

if(BUILD_TESTING)
    add_executable(unaligned_ptr_test test/unaligned_ptr_test.cpp)
    target_link_libraries(unaligned_ptr_test PRIVATE unaligned)
    target_include_directories(unaligned_ptr_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/test")
    target_compile_options(unaligned_ptr_test PRIVATE -Wall -Wextra)
    if(SERPENT_TEST_SANITIZE)
        target_compile_options(unaligned_ptr_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(unaligned_ptr_test PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME unaligned_ptr COMMAND unaligned_ptr_test)
endif()
