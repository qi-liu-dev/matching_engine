#pragma once

#include <chrono>
#include <utility>

namespace matching_engine::benchmark {

template <typename Result> struct TimedOperation {
  std::chrono::nanoseconds elapsed;
  Result result;
};

template <typename Clock, typename Operation>
[[nodiscard]] auto measure_operation(Operation &&operation) {
  const auto start = Clock::now();
  auto result = std::forward<Operation>(operation)();
  const auto finish = Clock::now();
  return TimedOperation<decltype(result)>{
      .elapsed =
          std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start),
      .result = std::move(result)};
}

} // namespace matching_engine::benchmark
