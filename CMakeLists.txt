cmake_minimum_required(VERSION 3.20)

project(
  matching_engine
  VERSION 0.1.0
  DESCRIPTION "Educational C++20 limit order book and matching engine"
  LANGUAGES CXX)

option(MATCHING_ENGINE_BUILD_TESTS "Build matching_engine tests" ON)
option(MATCHING_ENGINE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MATCHING_ENGINE_BUILD_BENCHMARKS "Build matching_engine benchmarks" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

add_library(matching_engine STATIC src/matching_engine.cpp)
add_library(matching_engine::matching_engine ALIAS matching_engine)

target_compile_features(matching_engine PUBLIC cxx_std_20)
set_target_properties(matching_engine PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(
  matching_engine
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

matching_engine_enable_warnings(matching_engine)
matching_engine_enable_sanitizers(matching_engine)

if(MATCHING_ENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MATCHING_ENGINE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
