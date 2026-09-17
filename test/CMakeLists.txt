# One executable per test file, each with its own main, driven by ctest:
#
#   cmake -S . -B build && cmake --build build
#   ctest --test-dir build --output-on-failure

# Format-level suites, which name no types of their own.
set(TESTS
    bjdata_scalar
    bjdata_container
    bjdata_ndarray
    bjdata_notation
    bjdata_writer
    json_reader
    json_real_read
    json_string_scan
    real_text
    json_indexed
    json_walking
    bjdata_walking
    fixture
    manual
    value)

# Suites that define types the way types are meant to be defined, by annotation.
if(SERPENT_HAS_REFLECTION_SUPPORT)
    list(APPEND TESTS json_writer json_shapes json_bounded_write serializer reflect)
endif()

foreach(TEST_NAME IN LISTS TESTS)
    add_executable(${TEST_NAME}_test "${TEST_NAME}_test.cpp")
    target_link_libraries(${TEST_NAME}_test PRIVATE serpent)
    target_include_directories(${TEST_NAME}_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_options(${TEST_NAME}_test PRIVATE -Wall -Wextra)
    target_compile_definitions(${TEST_NAME}_test PRIVATE
        SERPENT_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures")

    if(SERPENT_TEST_SANITIZE)
        target_compile_options(${TEST_NAME}_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${TEST_NAME}_test PRIVATE -fsanitize=address,undefined)
    endif()

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME}_test)
endforeach()

# The same check of a real's text against Żmij built the other way: without its table of powers
# of ten, which is how it builds itself under -Os and what SERPENT_ZMIJ_OPTIMIZE_SIZE asks for.
if(SERPENT_USE_ZMIJ)
    serpent_add_zmij(serpent_zmij_small_for_test ON)
    add_executable(real_text_small_test real_text_test.cpp)
    target_link_libraries(real_text_small_test PRIVATE serpent_headers serpent_zmij_small_for_test)
    target_include_directories(real_text_small_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(real_text_small_test PRIVATE SERPENT_USE_ZMIJ=1)
    target_compile_options(real_text_small_test PRIVATE -Wall -Wextra)
    add_test(NAME real_text_small COMMAND real_text_small_test)
endif()

# An integer's text, produced each of the ways SERPENT_INTEGER_TABLE chooses between. Not linked
# to the library target, which would fix the setting to the one this build was configured with.
foreach(TABLE IN ITEMS 0 1 2)
    add_executable(integer_text_${TABLE}_test integer_text_test.cpp)
    target_include_directories(integer_text_${TABLE}_test PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_features(integer_text_${TABLE}_test PRIVATE cxx_std_23)
    target_compile_definitions(integer_text_${TABLE}_test PRIVATE SERPENT_INTEGER_TABLE=${TABLE})
    target_compile_options(integer_text_${TABLE}_test PRIVATE -Wall -Wextra -O2)
    if(SERPENT_TEST_SANITIZE)
        target_compile_options(integer_text_${TABLE}_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(integer_text_${TABLE}_test PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME integer_text_${TABLE} COMMAND integer_text_${TABLE}_test)
endforeach()

# The reflected path is a separate binary because it needs a newer standard and a compiler
# flag; the rest of the suite must keep working without either.
if(SERPENT_HAS_REFLECTION_SUPPORT)
    # What the compiler can see about a type's wire form, it must refuse to let you get wrong.
    # Each of these is expected to FAIL to build - the diagnostic is the thing under test - so
    # they are excluded from the default build and only this suite asks for them.
    #
    #   reflect_conflict    a type both annotated and hand-written, which would rank silently
    #   enum_incomplete     a table that has fallen behind the enumeration it is for
    #   duplicate_key       two members that are the same key on the wire
    #   enum_ambiguous      two enumerators that are the same value on the wire
    #   annotation_misuse   an annotation that cannot mean anything where it is
    foreach(REFUSED IN ITEMS reflect_conflict enum_incomplete duplicate_key enum_ambiguous annotation_misuse)
        add_executable(${REFUSED}_test EXCLUDE_FROM_ALL "${REFUSED}_test.cpp")
        target_link_libraries(${REFUSED}_test PRIVATE serpent)
        add_test(NAME ${REFUSED}
                COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target ${REFUSED}_test)
        set_tests_properties(${REFUSED} PROPERTIES WILL_FAIL TRUE)
    endforeach()
endif()

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
