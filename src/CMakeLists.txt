cmake_minimum_required(VERSION 3.20)
project(brrr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# MinGW: link the C++ runtime statically, so a different libstdc++ DLL earlier on
# PATH (e.g. Git's) cannot be picked up at run time and crash the tools.
if(MINGW)
  add_link_options(-static-libgcc -static-libstdc++
                   -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic)
endif()

# ---- the core: the IR, its text form, the vocabulary, the solver, the checks ------
# This is the API everything else is written against.
add_library(brrr_core
  ir/ir.cpp
  ir/text.cpp
  schema/schema.cpp
  symbolic/symbolic.cpp
  symbolic/expr_io.cpp
  symbolic/facts.cpp
  symbolic/z3_backend.cpp
  verify/check.cpp
  sema/sema.cpp
  interop/affine.cpp
  io/json.cpp
  connectors/connectors.cpp
  types/types.cpp
  types/type_parse.cpp
  types/infer.cpp
)
target_include_directories(brrr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(brrr_core PRIVATE -Wall -Wextra)

# ---- Z3: optional; without it the local solver decides alone ---------------------
option(BRRR_Z3 "Use Z3 behind the symbolic layer, when found" ON)
set(BRRR_Z3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/z3" CACHE PATH
    "A Z3 distribution: include/z3.h and lib/ (libz3.so, libz3.dylib, or libz3.dll)")
if(BRRR_Z3)
  find_path(Z3_INCLUDE_DIR z3.h HINTS "${BRRR_Z3_ROOT}/include")
  find_library(Z3_LIBRARY NAMES z3 libz3 HINTS "${BRRR_Z3_ROOT}/lib" "${BRRR_Z3_ROOT}/bin")
  if(NOT Z3_LIBRARY AND WIN32)  # a DLL with no import library: GNU ld links it directly
    find_file(Z3_LIBRARY libz3.dll HINTS "${BRRR_Z3_ROOT}/lib" "${BRRR_Z3_ROOT}/bin")
  endif()
  if(Z3_INCLUDE_DIR AND Z3_LIBRARY)
    message(STATUS "Brrr: Z3 at ${Z3_LIBRARY}")
    target_compile_definitions(brrr_core PUBLIC BRRR_HAS_Z3)
    target_include_directories(brrr_core PRIVATE ${Z3_INCLUDE_DIR})
    target_link_libraries(brrr_core PUBLIC ${Z3_LIBRARY})
    if(WIN32)  # the tools find DLLs next to themselves
      get_filename_component(Z3_LIB_DIR ${Z3_LIBRARY} DIRECTORY)
      file(GLOB Z3_RUNTIME_DLLS "${Z3_LIB_DIR}/*.dll")
      file(COPY ${Z3_RUNTIME_DLLS} DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
    endif()
  else()
    message(STATUS "Brrr: Z3 not found (set BRRR_Z3_ROOT); the local solver decides alone")
  endif()
endif()

# ---- passes: one .cpp file per pass, each registering itself ----------------------
# An OBJECT library, so every pass is linked even though nothing names it.
file(GLOB BRRR_PASS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/passes/*.cpp)
add_library(brrr_passes OBJECT ${BRRR_PASS_SOURCES})
target_link_libraries(brrr_passes PUBLIC brrr_core)
target_compile_options(brrr_passes PRIVATE -Wall -Wextra)

# ---- the tool: the CLI and the run store --------------------------------------------
add_executable(brrr tools/brrr_main.cpp pipeline/runs.cpp)
target_link_libraries(brrr PRIVATE brrr_core brrr_passes)
target_compile_options(brrr PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME core
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh
          $<TARGET_FILE:brrr> ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases)

add_executable(brrr_types_test tests/type_inference_test.cpp)
target_link_libraries(brrr_types_test PRIVATE brrr_core)
add_test(NAME types COMMAND brrr_types_test)

add_executable(brrr_symbolic_test tests/symbolic_test.cpp)
target_link_libraries(brrr_symbolic_test PRIVATE brrr_core)
add_test(NAME symbolic COMMAND brrr_symbolic_test)

add_executable(brrr_connector_test tests/connector_test.cpp)
target_link_libraries(brrr_connector_test PRIVATE brrr_core)
add_test(NAME connector COMMAND brrr_connector_test)
