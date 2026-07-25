#include "benchmark_timing.hpp"

#include <chrono>

namespace {

struct FakeClock {
  using duration = std::chrono::nanoseconds;
  using time_point = std::chrono::time_point<FakeClock, duration>;

  static time_point now() noexcept { return current; }

  static inline time_point current{};
};

} // namespace

int main() {
  FakeClock::current = FakeClock::time_point{std::chrono::nanoseconds{10}};
  const auto timed =
      matching_engine::benchmark::measure_operation<FakeClock>([] {
        FakeClock::current =
            FakeClock::time_point{std::chrono::nanoseconds{25}};
        return 42;
      });

  FakeClock::current = FakeClock::time_point{std::chrono::nanoseconds{1'000}};
  const int checksum = timed.result * 2;

  return timed.elapsed == std::chrono::nanoseconds{15} && checksum == 84 ? 0
                                                                         : 1;
}
