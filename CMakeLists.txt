cmake_minimum_required(VERSION 3.20)

project(serpent CXX)

# Header only. The include paths deliberately match sunrise-firmware's
# lib/common/include/nonstd, so these headers can later be dropped in there unchanged -
# which is why the namespaces stay nonstd:: even though the package is named serpent.
add_library(serpent INTERFACE)
add_library(serpent::serpent ALIAS serpent)

target_include_directories(serpent INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(serpent INTERFACE cxx_std_23)

if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(test)
endif()
