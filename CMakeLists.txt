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

# Header only.
add_library(serpent INTERFACE)
add_library(serpent::serpent ALIAS serpent)

target_include_directories(serpent INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(serpent INTERFACE unaligned::unaligned)

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
