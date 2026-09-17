# Off by default: it pulls in other libraries and 4.6 MB of corpus, none of which the library
# itself needs. tools/bench.sh configures, builds and runs all of this and prints the summary;
# by hand it is
#
#   cmake -S . -B build-bench -DSERPENT_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-bench --target bench

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

# Timing is nanobench's: one header, pinned, with its implementation compiled once here.
set(NANOBENCH_DIR "${SINGLE_DIR}/nanobench")
if(NOT EXISTS "${NANOBENCH_DIR}/nanobench.h")
    message(STATUS "Fetching nanobench")
    file(DOWNLOAD "https://raw.githubusercontent.com/martinus/nanobench/v4.3.11/src/include/nanobench.h"
            "${NANOBENCH_DIR}/nanobench.h"
            EXPECTED_HASH SHA256=7d20cc0daed3f56341ccd9b5c4b1b25b2f064d76638a5ba0304a5795a32cc1b6
            STATUS NANOBENCH_STATUS)
    list(GET NANOBENCH_STATUS 0 NANOBENCH_CODE)
    if(NOT NANOBENCH_CODE EQUAL 0)
        file(REMOVE "${NANOBENCH_DIR}/nanobench.h")
        message(FATAL_ERROR "Could not fetch nanobench: ${NANOBENCH_STATUS}")
    endif()
endif()

# Everything that is the same whichever way serpent is configured, built once.
add_library(serpent_bench_others STATIC nanobench.cpp "${SINGLE_DIR}/simdjson/simdjson.cpp" "${SINGLE_DIR}/yyjson/yyjson.c")
target_include_directories(serpent_bench_others PUBLIC "${NANOBENCH_DIR}" "${SINGLE_DIR}")
target_compile_options(serpent_bench_others PRIVATE -O3 -DNDEBUG)
target_compile_features(serpent_bench_others PUBLIC cxx_std_20)

set(RESULTS_DIR "${CMAKE_BINARY_DIR}/results")
file(MAKE_DIRECTORY "${RESULTS_DIR}")
set(BENCH_COMMANDS "")
set(BENCH_TARGETS "")

# The same program once per configuration of the library, so that what each switch in
# include/serpent/config.hpp buys is measured rather than remembered. Every switch is given a
# value in every configuration, so that none depends on what this build was configured with.
#
#   serpent_add_benchmark(<configuration> [<switch>=<value>...])     the rest as in `default`
set(BENCH_DEFAULT_SWITCHES
    SERPENT_USE_ZMIJ=1
    SERPENT_BOUNDED_OBJECT_WRITE=1
    SERPENT_INTEGER_TABLE=1
    SERPENT_USE_FAST_FLOAT=1
    SERPENT_ZMIJ_OPTIMIZE_SIZE=0)

# Żmij both ways, whichever way this build has it, since a configuration may ask for either.
serpent_add_zmij(serpent_bench_zmij_fast OFF)
serpent_add_zmij(serpent_bench_zmij_small ON)
target_compile_options(serpent_bench_zmij_fast PRIVATE -O3 -DNDEBUG)
target_compile_options(serpent_bench_zmij_small PRIVATE -O3 -DNDEBUG)

function(serpent_add_benchmark CONFIGURATION)
    set(SWITCHES ${BENCH_DEFAULT_SWITCHES})
    foreach(OVERRIDE IN LISTS ARGN)
        string(REGEX REPLACE "=.*" "" SWITCH_NAME "${OVERRIDE}")
        list(FILTER SWITCHES EXCLUDE REGEX "^${SWITCH_NAME}=")
        list(APPEND SWITCHES "${OVERRIDE}")
    endforeach()

    set(NAME "serpent_bench_${CONFIGURATION}")
    add_executable(${NAME} main.cpp)
    target_link_libraries(${NAME} PRIVATE serpent::headers glaze::glaze serpent_bench_others)
    if("SERPENT_USE_ZMIJ=1" IN_LIST SWITCHES)
        if("SERPENT_ZMIJ_OPTIMIZE_SIZE=1" IN_LIST SWITCHES)
            target_link_libraries(${NAME} PRIVATE serpent_bench_zmij_small)
        else()
            target_link_libraries(${NAME} PRIVATE serpent_bench_zmij_fast)
        endif()
    endif()
    target_include_directories(${NAME} PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${rapidjson_SOURCE_DIR}/include")
    target_compile_definitions(${NAME} PRIVATE
        SERPENT_CORPUS_DIR="${CORPUS_DIR}"
        SERPENT_BENCH_CONFIGURATION="${CONFIGURATION}"
        ${SWITCHES})
    target_compile_options(${NAME} PRIVATE -O3 -DNDEBUG)

    list(APPEND BENCH_COMMANDS COMMAND ${NAME} --json "${RESULTS_DIR}/${CONFIGURATION}.json")
    list(APPEND BENCH_TARGETS ${NAME})
    set(BENCH_COMMANDS "${BENCH_COMMANDS}" PARENT_SCOPE)
    set(BENCH_TARGETS "${BENCH_TARGETS}" PARENT_SCOPE)
endfunction()

# As the library comes; with everything off; with the one switch that costs an image real space
# turned up; and with the tables an image may not have room for left out.
serpent_add_benchmark(default)
serpent_add_benchmark(plain
    SERPENT_USE_ZMIJ=0 SERPENT_BOUNDED_OBJECT_WRITE=0 SERPENT_INTEGER_TABLE=0 SERPENT_USE_FAST_FLOAT=0)
serpent_add_benchmark(large SERPENT_INTEGER_TABLE=2)
serpent_add_benchmark(small SERPENT_ZMIJ_OPTIMIZE_SIZE=1 SERPENT_USE_FAST_FLOAT=0)

# `default` with one switch off at a time, which is what says what that switch is worth. Several
# more builds of a slow translation unit, so only on request: tools/bench.sh --ablations.
option(SERPENT_BENCH_ABLATIONS "Also build the benchmark with each switch turned off on its own" OFF)
if(SERPENT_BENCH_ABLATIONS)
    serpent_add_benchmark(without_zmij SERPENT_USE_ZMIJ=0)
    serpent_add_benchmark(without_bounded_write SERPENT_BOUNDED_OBJECT_WRITE=0)
    serpent_add_benchmark(without_integer_table SERPENT_INTEGER_TABLE=0)
    serpent_add_benchmark(without_fast_float SERPENT_USE_FAST_FLOAT=0)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    list(APPEND BENCH_COMMANDS COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/bench-summary.py" "${RESULTS_DIR}")
endif()

add_custom_target(serpent_benchmarks DEPENDS ${BENCH_TARGETS})
add_custom_target(bench ${BENCH_COMMANDS} DEPENDS ${BENCH_TARGETS} USES_TERMINAL)
