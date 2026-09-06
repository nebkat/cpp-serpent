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
    json_reader
    json_writer
    serializer
    fixture)

foreach(TEST_NAME IN LISTS TESTS)
    add_executable(${TEST_NAME}_test "${TEST_NAME}_test.cpp")
    target_link_libraries(${TEST_NAME}_test PRIVATE serpent)
    target_include_directories(${TEST_NAME}_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_options(${TEST_NAME}_test PRIVATE -Wall -Wextra)
    target_compile_definitions(${TEST_NAME}_test PRIVATE
        SERPENT_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures")

    if(BJDATA_TEST_SANITIZE)
        target_compile_options(${TEST_NAME}_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${TEST_NAME}_test PRIVATE -fsanitize=address,undefined)
    endif()

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME}_test)
endforeach()

# The library must also compile with exceptions off, where only the total accessors exist.
add_executable(no_exceptions_test no_exceptions_test.cpp)
target_link_libraries(no_exceptions_test PRIVATE serpent)
target_include_directories(no_exceptions_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_options(no_exceptions_test PRIVATE -Wall -Wextra -fno-exceptions)
add_test(NAME no_exceptions COMMAND no_exceptions_test)

# Every header must compile on its own. A missing include is otherwise invisible, because
# whichever header a test happened to include first supplied it.
file(GLOB_RECURSE PUBLIC_HEADERS
     RELATIVE "${CMAKE_CURRENT_LIST_DIR}/../include"
     "${CMAKE_CURRENT_LIST_DIR}/../include/*.hpp")

set(STANDALONE_SOURCES)
foreach(HEADER IN LISTS PUBLIC_HEADERS)
    string(REGEX REPLACE "[/.]" "_" STEM "${HEADER}")
    set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/standalone_${STEM}.cpp")
    file(WRITE "${GENERATED}" "#include <${HEADER}>\n")
    list(APPEND STANDALONE_SOURCES "${GENERATED}")
endforeach()

add_library(standalone_headers OBJECT ${STANDALONE_SOURCES})
target_link_libraries(standalone_headers PRIVATE serpent)
target_compile_options(standalone_headers PRIVATE -Wall -Wextra)
