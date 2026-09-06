# One executable per test file, each with its own main, driven by ctest:
#
#   cmake -S . -B build && cmake --build build
#   ctest --test-dir build --output-on-failure

option(BJDATA_TEST_SANITIZE "Build the tests with AddressSanitizer and UndefinedBehaviorSanitizer" ON)

set(TESTS
    unaligned_ptr
    bjdata_scalar
    bjdata_container
    bjdata_ndarray
    bjdata_notation
    bjdata_writer
    bjdata_serializer
    bjdata_json
    bjdata_fixture)

foreach(TEST_NAME IN LISTS TESTS)
    add_executable(${TEST_NAME}_test "${TEST_NAME}_test.cpp")
    target_link_libraries(${TEST_NAME}_test PRIVATE bjdata)
    target_include_directories(${TEST_NAME}_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_options(${TEST_NAME}_test PRIVATE -Wall -Wextra)
    target_compile_definitions(${TEST_NAME}_test PRIVATE
        BJDATA_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures")

    if(BJDATA_TEST_SANITIZE)
        target_compile_options(${TEST_NAME}_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${TEST_NAME}_test PRIVATE -fsanitize=address,undefined)
    endif()

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME}_test)
endforeach()

# The library must also compile with exceptions off, where only the total accessors exist.
add_executable(no_exceptions_test no_exceptions_test.cpp)
target_link_libraries(no_exceptions_test PRIVATE bjdata)
target_include_directories(no_exceptions_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_options(no_exceptions_test PRIVATE -Wall -Wextra -fno-exceptions)
add_test(NAME no_exceptions COMMAND no_exceptions_test)
