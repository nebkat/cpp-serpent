# Off by default: it pulls in another library and 4.6 MB of corpus, neither of which the
# library itself needs.
#
#   cmake -S . -B build-bench -DSERPENT_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-bench && ./build-bench/benchmark/serpent_bench <corpus dir>

# The single-header release rather than the repository: cloning it is hundreds of megabytes of
# test data to get one file.
set(OTHER_DIR "${CMAKE_CURRENT_BINARY_DIR}/other/nlohmann")
if(NOT EXISTS "${OTHER_DIR}/json.hpp")
    message(STATUS "Fetching comparison library")
    file(DOWNLOAD "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
            "${OTHER_DIR}/json.hpp"
            EXPECTED_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
            SHOW_PROGRESS STATUS OTHER_STATUS)
    list(GET OTHER_STATUS 0 OTHER_CODE)
    if(NOT OTHER_CODE EQUAL 0)
        file(REMOVE "${OTHER_DIR}/json.hpp")
        message(FATAL_ERROR "Could not fetch the comparison library: ${OTHER_STATUS}")
    endif()
endif()

# The other comparisons. Single-file distributions where one exists, so a benchmark run does
# not clone hundreds of megabytes; the two header-only repositories are shallow clones.
set(SINGLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/other")
foreach(PAIR
        "simdjson/simdjson.h|https://raw.githubusercontent.com/simdjson/simdjson/master/singleheader/simdjson.h"
        "simdjson/simdjson.cpp|https://raw.githubusercontent.com/simdjson/simdjson/master/singleheader/simdjson.cpp"
        "yyjson/yyjson.h|https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.h"
        "yyjson/yyjson.c|https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.c")
    string(REPLACE "|" ";" PARTS "${PAIR}")
    list(GET PARTS 0 SINGLE_NAME)
    list(GET PARTS 1 SINGLE_URL)
    if(NOT EXISTS "${SINGLE_DIR}/${SINGLE_NAME}")
        message(STATUS "Fetching comparison library: ${SINGLE_NAME}")
        file(DOWNLOAD "${SINGLE_URL}" "${SINGLE_DIR}/${SINGLE_NAME}" STATUS SINGLE_STATUS)
        list(GET SINGLE_STATUS 0 SINGLE_CODE)
        if(NOT SINGLE_CODE EQUAL 0)
            file(REMOVE "${SINGLE_DIR}/${SINGLE_NAME}")
            message(FATAL_ERROR "Could not fetch ${SINGLE_NAME}: ${SINGLE_STATUS}")
        endif()
    endif()
endforeach()

# One of the comparison libraries is C, and the project itself declares only CXX.
enable_language(C)

include(FetchContent)
# SOURCE_SUBDIR names a directory with no CMakeLists so the sources are fetched without
# configuring the project - it is header-only and its own build only adds tests and examples.
FetchContent_Declare(rapidjson
        GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR headers-only)
FetchContent_Declare(glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG main
        GIT_SHALLOW TRUE)
set(GLAZE_BUILD_TESTS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(rapidjson glaze)

# The corpus the published C++ JSON benchmarks use. Downloaded rather than vendored: it is
# someone else's data and it is larger than the library.
set(CORPUS_DIR "${CMAKE_CURRENT_BINARY_DIR}/corpus")
file(MAKE_DIRECTORY "${CORPUS_DIR}")
foreach(PAIR
        "canada.json|https://raw.githubusercontent.com/miloyip/nativejson-benchmark/master/data/canada.json"
        "citm_catalog.json|https://raw.githubusercontent.com/simdjson/simdjson/master/jsonexamples/citm_catalog.json"
        "twitter.json|https://raw.githubusercontent.com/simdjson/simdjson/master/jsonexamples/twitter.json")
    string(REPLACE "|" ";" PARTS "${PAIR}")
    list(GET PARTS 0 CORPUS_NAME)
    list(GET PARTS 1 CORPUS_URL)
    if(NOT EXISTS "${CORPUS_DIR}/${CORPUS_NAME}")
        message(STATUS "Fetching benchmark corpus: ${CORPUS_NAME}")
        file(DOWNLOAD "${CORPUS_URL}" "${CORPUS_DIR}/${CORPUS_NAME}" SHOW_PROGRESS STATUS CORPUS_STATUS)
        list(GET CORPUS_STATUS 0 CORPUS_CODE)
        if(NOT CORPUS_CODE EQUAL 0)
            file(REMOVE "${CORPUS_DIR}/${CORPUS_NAME}")
            message(FATAL_ERROR "Could not fetch ${CORPUS_NAME}: ${CORPUS_STATUS}")
        endif()
    endif()
endforeach()

add_executable(serpent_bench main.cpp "${SINGLE_DIR}/simdjson/simdjson.cpp" "${SINGLE_DIR}/yyjson/yyjson.c")
target_link_libraries(serpent_bench PRIVATE serpent glaze::glaze)
target_include_directories(serpent_bench PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${SINGLE_DIR}" "${rapidjson_SOURCE_DIR}/include")
target_compile_definitions(serpent_bench PRIVATE SERPENT_CORPUS_DIR="${CORPUS_DIR}")
target_compile_options(serpent_bench PRIVATE -O3 -DNDEBUG)

# The same program against the library with its options off, so that what each option buys is
# measured rather than remembered.
add_executable(serpent_bench_plain main.cpp "${SINGLE_DIR}/simdjson/simdjson.cpp" "${SINGLE_DIR}/yyjson/yyjson.c")
target_link_libraries(serpent_bench_plain PRIVATE serpent::plain glaze::glaze)
target_include_directories(serpent_bench_plain PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${SINGLE_DIR}" "${rapidjson_SOURCE_DIR}/include")
target_compile_definitions(serpent_bench_plain PRIVATE SERPENT_CORPUS_DIR="${CORPUS_DIR}")
target_compile_options(serpent_bench_plain PRIVATE -O3 -DNDEBUG)

add_custom_target(bench
        COMMAND serpent_bench "${CORPUS_DIR}"
        COMMAND serpent_bench_plain "${CORPUS_DIR}"
        DEPENDS serpent_bench serpent_bench_plain
        USES_TERMINAL)
