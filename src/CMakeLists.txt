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
  ir/sugar.cpp
  io/json.cpp
  io/c_like.cpp
  passes/demo_fuse.cpp
  passes/fold_reshape.cpp
  schema/schema.cpp
  types/types.cpp
  types/infer.cpp
  symbolic/symbolic.cpp
  symbolic/expr_io.cpp
  symbolic/facts.cpp
  verify/check.cpp
  connectors/connectors.cpp
)
target_include_directories(bridgeir_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bridgeir_core PRIVATE -Wall -Wextra)

add_executable(bridgeir tools/bridgeir_main.cpp)
target_link_libraries(bridgeir PRIVATE bridgeir_core)

enable_testing()
add_test(NAME core
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh
          $<TARGET_FILE:bridgeir> ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases)

add_executable(bridgeir_types_test tests/type_inference_test.cpp)
target_link_libraries(bridgeir_types_test PRIVATE bridgeir_core)
add_test(NAME types COMMAND bridgeir_types_test)

add_executable(bridgeir_symbolic_test tests/symbolic_test.cpp)
target_link_libraries(bridgeir_symbolic_test PRIVATE bridgeir_core)
add_test(NAME symbolic COMMAND bridgeir_symbolic_test)

add_executable(bridgeir_connector_test tests/connector_test.cpp)
target_link_libraries(bridgeir_connector_test PRIVATE bridgeir_core)
add_test(NAME connector COMMAND bridgeir_connector_test)
