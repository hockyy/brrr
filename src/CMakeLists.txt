cmake_minimum_required(VERSION 3.20)
project(bridgeir LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(bridgeir_core
  ir/ir.cpp
  ir/text.cpp
  passes/demo_fuse.cpp
)
target_include_directories(bridgeir_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bridgeir_core PRIVATE -Wall -Wextra)

add_executable(bridgeir tools/bridgeir_main.cpp)
target_link_libraries(bridgeir PRIVATE bridgeir_core)

enable_testing()
add_test(NAME core
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh
          $<TARGET_FILE:bridgeir> ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases)
