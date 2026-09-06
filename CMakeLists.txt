cmake_minimum_required(VERSION 3.20)

project(bjdata CXX)

# Header only. The include paths deliberately match sunrise-firmware's
# lib/common/include/nonstd, so these headers can later be dropped in there unchanged.
add_library(bjdata INTERFACE)
add_library(bjdata::bjdata ALIAS bjdata)

target_include_directories(bjdata INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(bjdata INTERFACE cxx_std_23)

if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(test)
endif()
