cmake_minimum_required(VERSION 3.20)

project(serpent CXX)

# PROJECT_IS_TOP_LEVEL is true of any project() call, including one made inside a build that
# merely includes us - an ESP-IDF component, say. Comparing the source directories asks the
# question actually meant: is this the build being configured, or somebody's dependency? Getting
# it wrong cross-compiles our host tests for their target, with sanitizers on.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SERPENT_IS_MAIN_BUILD ON)
else()
    set(SERPENT_IS_MAIN_BUILD OFF)
endif()

option(SERPENT_BUILD_TESTS "Build the host tests and examples" ${SERPENT_IS_MAIN_BUILD})
option(SERPENT_TEST_SANITIZE "Build the tests with AddressSanitizer and UndefinedBehaviorSanitizer" ON)
option(SERPENT_BUILD_BENCHMARKS "Build the benchmarks, which fetch another library and a test corpus" OFF)
option(SERPENT_USE_ZMIJ "Write reals with the bundled Żmij rather than std::to_chars; builds one source file" ON)

# The optimisations that have a plainer alternative, each of which include/serpent/config.hpp
# describes. All on unless turned off; the target serpent::plain has every one of them off.
option(SERPENT_BOUNDED_OBJECT_WRITE "Write runs of boolean and number members into room claimed once" ON)
option(SERPENT_USE_FAST_FLOAT "Read reals with the bundled fast_float rather than std::from_chars" ON)
set(SERPENT_HEADER_SWITCHES SERPENT_BOUNDED_OBJECT_WRITE SERPENT_USE_FAST_FLOAT)

set(SERPENT_INTEGER_TABLE 1 CACHE STRING
    "How integers are written: 0 std::to_chars, 1 a 400-byte digit table, 2 a 40 KB one")
set_property(CACHE SERPENT_INTEGER_TABLE PROPERTY STRINGS 0 1 2)

# Reading and writing unaligned values has nothing to do with serialization, so it is its own
# library. Fetched before BUILD_TESTING is set, so it does not build its tests here.
include(FetchContent)
# Consumed as a dependency, so its own tests are none of this build's business - it has its own
# CI for those. Set directly rather than through BUILD_TESTING, which it may set for itself.
set(UNALIGNED_BUILD_TESTS OFF CACHE INTERNAL "")

# It declares the same C++ minimum, and links through to us, so one answer has to cover both -
# otherwise turning ours off still leaves a -std flag on the consumer's command line.
option(SERPENT_SET_CXX_STANDARD "Declare the C++23 minimum on consumers" ON)
set(UNALIGNED_SET_CXX_STANDARD ${SERPENT_SET_CXX_STANDARD} CACHE INTERNAL "")
FetchContent_Declare(unaligned
        GIT_REPOSITORY git@github.com:nebkat/cpp-unaligned.git
        GIT_TAG v0.1.1
        GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(unaligned)

if(SERPENT_BUILD_TESTS)
    enable_testing()
    set(BUILD_TESTING ON)
endif()

# Reflection lets a type name its own fields. It needs P2996, P1306 and P3394 together, which
# today means GCC 16 with -freflection, so it is detected rather than assumed.
#
# Only for our own build. A consumer decides its own standard and flags, and probing - let alone
# adding flags - inside their configure would be presumptuous and, cross-compiling, wrong.
if(SERPENT_BUILD_TESTS)
include(CheckCXXSourceCompiles)
set(SERPENT_REFLECTION_FLAGS -std=c++26 -freflection)
set(CMAKE_REQUIRED_FLAGS "-std=c++26 -freflection")
check_cxx_source_compiles([==[
    #include <meta>
    struct marker {};
    struct [[=marker{}]] tagged { [[=marker{}]] int field; };
    consteval bool probe() {
        bool named = false;
        template for (constexpr auto member : std::define_static_array(
                std::meta::nonstatic_data_members_of(^^tagged, std::meta::access_context::current()))) {
            named = std::meta::identifier_of(member) == "field"
                    && !std::meta::annotations_of_with_type(member, ^^marker).empty();
        }
        return named && !std::meta::annotations_of_with_type(^^tagged, ^^marker).empty();
    }
    static_assert(probe());
    int main() { return 0; }
]==] SERPENT_HAS_REFLECTION_SUPPORT)
unset(CMAKE_REQUIRED_FLAGS)

# Reflection is the way a type is defined; the macro and function forms are what you fall back
# to. So everything here is built with it where the compiler has it, and the parts that need it
# are simply not built where it does not.
if(SERPENT_HAS_REFLECTION_SUPPORT)
    # C++ only: a comparison library may bring a C file with it, and neither the standard nor
    # the flag means anything there.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${SERPENT_REFLECTION_FLAGS}>")
    add_link_options(${SERPENT_REFLECTION_FLAGS})
endif()
endif()

# The headers and what they need, with nothing said about the switches in config.hpp. What
# `serpent` and `serpent::plain` are each built from, and what the benchmarks configure for
# themselves one way after another.
add_library(serpent_headers INTERFACE)
add_library(serpent::headers ALIAS serpent_headers)
target_include_directories(serpent_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(serpent_headers INTERFACE unaligned::unaligned)

# The library as this build's options configure it.
add_library(serpent INTERFACE)
add_library(serpent::serpent ALIAS serpent)
target_link_libraries(serpent INTERFACE serpent_headers)

foreach(SWITCH IN LISTS SERPENT_HEADER_SWITCHES)
    target_compile_definitions(serpent INTERFACE ${SWITCH}=$<BOOL:${${SWITCH}}>)
endforeach()
target_compile_definitions(serpent INTERFACE SERPENT_INTEGER_TABLE=${SERPENT_INTEGER_TABLE})

# Writing a real is mostly finding its shortest digits, and Żmij - a copy of which is kept under
# include/serpent/external, see tools/vendor-zmij.sh - does the whole job in about a third of the
# time std::to_chars and our own layout take. The text is the same either way.
#
# It is the one part of this library that is not a header: its source is built here, once, into
# a library serpent links. Ported into a header instead it would cost every translation unit that
# writes JSON about half a second, which is more than the rest of this library costs to include.
# Turn the option off to go without it and stay header-only.
#
# SERPENT_ZMIJ_OPTIMIZE_SIZE is Żmij's own switch between its two builds: with its table of
# powers of ten, or computing them as it goes - 67 KB of code or 36 KB, built for arm64. AUTO leaves the choice to Żmij, which
# takes the small build when the compiler is optimising for size (-Os) and the fast one otherwise.
set(SERPENT_ZMIJ_OPTIMIZE_SIZE AUTO CACHE STRING
    "Build Żmij without its power-of-ten table: AUTO (only under -Os), ON or OFF")
set_property(CACHE SERPENT_ZMIJ_OPTIMIZE_SIZE PROPERTY STRINGS AUTO ON OFF)

function(serpent_add_zmij NAME SIZE_MODE)
    add_library(${NAME} STATIC "${PROJECT_SOURCE_DIR}/include/serpent/external/zmij/zmij.cc")
    target_compile_features(${NAME} PRIVATE cxx_std_20)
    set_target_properties(${NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(NOT SIZE_MODE STREQUAL "AUTO")
        target_compile_definitions(${NAME} PRIVATE SERPENT_ZMIJ_OPTIMIZE_SIZE=$<BOOL:${SIZE_MODE}>)
    endif()
endfunction()

if(SERPENT_USE_ZMIJ)
    serpent_add_zmij(serpent_zmij ${SERPENT_ZMIJ_OPTIMIZE_SIZE})
    add_library(serpent::zmij ALIAS serpent_zmij)
    target_link_libraries(serpent INTERFACE serpent_zmij)
    target_compile_definitions(serpent INTERFACE SERPENT_USE_ZMIJ=1)
endif()

# The library with every option off, whatever this build chose: headers only, and the standard
# library for anything an option would have replaced. For the benchmarks, which measure both.
add_library(serpent_plain INTERFACE)
add_library(serpent::plain ALIAS serpent_plain)
target_link_libraries(serpent_plain INTERFACE serpent_headers)
target_compile_definitions(serpent_plain INTERFACE SERPENT_USE_ZMIJ=0)
foreach(SWITCH IN LISTS SERPENT_HEADER_SWITCHES)
    target_compile_definitions(serpent_plain INTERFACE ${SWITCH}=0)
endforeach()
target_compile_definitions(serpent_plain INTERFACE SERPENT_INTEGER_TABLE=0)

# Declaring the minimum puts a -std flag on every consumer's command line, which is right for a
# CMake-managed build and wrong for one that sets the standard itself with raw flags - ESP-IDF
# passes -std=gnu++26, and whichever lands last wins. Turn this off there and set your own.
if(SERPENT_SET_CXX_STANDARD)
    target_compile_features(serpent INTERFACE cxx_std_23)
endif()

if(SERPENT_BUILD_TESTS)
    add_subdirectory(test)
    add_subdirectory(example)
    if(SERPENT_BUILD_BENCHMARKS)
        add_subdirectory(benchmark)
    endif()
endif()
