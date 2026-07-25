add_executable(matching_engine_benchmarks matching_benchmarks.cpp)

target_link_libraries(matching_engine_benchmarks
                      PRIVATE matching_engine::matching_engine)
target_compile_features(matching_engine_benchmarks PRIVATE cxx_std_20)
set_target_properties(matching_engine_benchmarks PROPERTIES CXX_EXTENSIONS OFF)

matching_engine_enable_warnings(matching_engine_benchmarks)
matching_engine_enable_sanitizers(matching_engine_benchmarks)

string(TOUPPER "${CMAKE_BUILD_TYPE}" matching_engine_build_type_upper)
set(matching_engine_config_flags_variable
    "CMAKE_CXX_FLAGS_${matching_engine_build_type_upper}")
set(matching_engine_benchmark_flags
    "${CMAKE_CXX_FLAGS} ${${matching_engine_config_flags_variable}}")
string(STRIP "${matching_engine_benchmark_flags}"
       matching_engine_benchmark_flags)

target_compile_definitions(
  matching_engine_benchmarks
  PRIVATE
    "MATCHING_ENGINE_BENCHMARK_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\""
    "MATCHING_ENGINE_BENCHMARK_COMPILER_ID=\"${CMAKE_CXX_COMPILER_ID}\""
    "MATCHING_ENGINE_BENCHMARK_COMPILER_VERSION=\"${CMAKE_CXX_COMPILER_VERSION}\""
    "MATCHING_ENGINE_BENCHMARK_FLAGS=\"${matching_engine_benchmark_flags}\"")

if(MATCHING_ENGINE_BUILD_TESTS)
  add_executable(matching_engine_benchmark_timing_tests
                 benchmark_timing_test.cpp)
  target_compile_features(matching_engine_benchmark_timing_tests
                          PRIVATE cxx_std_20)
  set_target_properties(matching_engine_benchmark_timing_tests
                        PROPERTIES CXX_EXTENSIONS OFF)
  matching_engine_enable_warnings(matching_engine_benchmark_timing_tests)
  matching_engine_enable_sanitizers(matching_engine_benchmark_timing_tests)
  add_test(NAME matching_engine_benchmark_timing_tests
           COMMAND matching_engine_benchmark_timing_tests)

  add_test(
    NAME matching_engine_benchmark_smoke
    COMMAND
      matching_engine_benchmarks --events 20 --repetitions 1 --warmup 0
      --mode all)
  set_tests_properties(
    matching_engine_benchmark_smoke
    PROPERTIES PASS_REGULAR_EXPRESSION
               "result workload=deep_book mode=instrumented_latency samples=20"
               FAIL_REGULAR_EXPRESSION "matching_engine_baseline")

  add_test(
    NAME matching_engine_benchmark_selected_workload
    COMMAND
      matching_engine_benchmarks --workload mixed_cancel --events 3
      --repetitions 1 --warmup 0 --mode throughput)
  set_tests_properties(
    matching_engine_benchmark_selected_workload
    PROPERTIES
      PASS_REGULAR_EXPRESSION
      "result workload=mixed_cancel mode=throughput measured_events=3"
      FAIL_REGULAR_EXPRESSION "mode=instrumented_latency|workload=non_marketable")

  add_test(NAME matching_engine_benchmark_invalid_arguments
           COMMAND matching_engine_benchmarks --events 0)
  set_tests_properties(matching_engine_benchmark_invalid_arguments
                       PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME matching_engine_benchmark_replay_limit
    COMMAND
      matching_engine_benchmarks --events 10000000 --repetitions 10 --warmup 1)
  set_tests_properties(matching_engine_benchmark_replay_limit
                       PROPERTIES WILL_FAIL TRUE)
endif()
