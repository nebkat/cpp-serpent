cmake_minimum_required(VERSION 3.20)

project(serpent CXX)

option(SERPENT_TEST_SANITIZE "Build the tests with AddressSanitizer and UndefinedBehaviorSanitizer" ON)
option(SERPENT_BUILD_BENCHMARKS "Build the benchmarks, which fetch another library and a test corpus" OFF)

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

# Reflection lets a type name its own fields. It needs P2996, P1306 and P3394 together, which
# today means GCC 16 with -freflection, so it is detected rather than assumed.
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
    add_compile_options(${SERPENT_REFLECTION_FLAGS})
    add_link_options(${SERPENT_REFLECTION_FLAGS})
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
    if(SERPENT_BUILD_BENCHMARKS)
        add_subdirectory(benchmark)
    endif()
endif()
