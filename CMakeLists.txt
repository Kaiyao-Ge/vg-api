cmake_minimum_required(VERSION 3.25)
project(vg VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(VG_ENABLE_METAL "Build the macOS Metal capability probe" OFF)
option(VG_ENABLE_VULKAN "Build the Linux Vulkan capability probe" OFF)
option(VG_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

if(VG_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(CTest)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

include(cmake/registration.cmake)
include(cmake/schemas.cmake)
include(cmake/targets.cmake)
if(BUILD_TESTING)
  include(cmake/tests.cmake)
endif()
