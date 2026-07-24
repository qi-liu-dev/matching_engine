#include "matching_engine/matching_engine.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using matching_engine::ErrorCode;
using matching_engine::MarketOrderRequest;
using matching_engine::MatchingEngine;
using matching_engine::OrderId;
using matching_engine::OrderRequest;
using matching_engine::Price;
using matching_engine::Quantity;
using matching_engine::Side;
using matching_engine::SubmitResult;
using matching_engine::Trade;

constexpr std::size_t default_event_count = 20'000U;
constexpr std::size_t default_repetitions = 3U;
constexpr std::size_t default_warmup_repetitions = 1U;
constexpr std::size_t maximum_event_count = 10'000'000U;
constexpr std::size_t maximum_repetition_count = 1'000U;
constexpr std::size_t maximum_replayed_event_count = 100'000'000U;

constexpr std::array<std::string_view, 5> workload_names{
    "non_marketable", "mixed_cancel", "highly_marketable", "single_price",
    "deep_book"};

enum class EventKind { Limit, Market, Cancel };
enum class MeasurementMode { All, Throughput, Latency };

struct Event {
  EventKind kind;
  OrderId id;
  Side side;
  Price price;
  Quantity quantity;
};

struct Workload {
  std::string_view name;
  std::string_view description;
  std::vector<Event> events;
};

struct Configuration {
  std::size_t event_count{default_event_count};
  std::size_t repetitions{default_repetitions};
  std::size_t warmup_repetitions{default_warmup_repetitions};
  std::string_view workload{"all"};
  MeasurementMode mode{MeasurementMode::All};
  bool show_help{false};
  bool list_workloads{false};
};

struct EventOutcome {
  ErrorCode error;
  std::uint64_t checksum;
};

struct TimedRun {
  std::chrono::nanoseconds elapsed;
  std::uint64_t checksum;
};

[[nodiscard]] bool is_known_workload(std::string_view name) {
  return std::find(workload_names.begin(), workload_names.end(), name) !=
         workload_names.end();
}

[[nodiscard]] std::size_t parse_size(std::string_view text,
                                     std::string_view option, bool allow_zero) {
  std::uint64_t value{};
  const char *begin = text.data();
  const char *end = text.data() + text.size();
  const auto [position, error] = std::from_chars(begin, end, value);

  if (error != std::errc{} || position != end || (!allow_zero && value == 0U) ||
      value > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument{"invalid value for " + std::string{option}};
  }

  return static_cast<std::size_t>(value);
}

[[nodiscard]] std::string_view require_value(int argc, char **argv, int &index,
                                             std::string_view option) {
  if (index + 1 >= argc) {
    throw std::invalid_argument{"missing value for " + std::string{option}};
  }

  ++index;
  return argv[index];
}

[[nodiscard]] Configuration parse_configuration(int argc, char **argv) {
  Configuration configuration;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help") {
      configuration.show_help = true;
    } else if (argument == "--list") {
      configuration.list_workloads = true;
    } else if (argument == "--events") {
      const std::string_view value = require_value(argc, argv, index, argument);
      configuration.event_count = parse_size(value, argument, false);
      if (configuration.event_count > maximum_event_count) {
        throw std::invalid_argument{"--events exceeds safety limit"};
      }
    } else if (argument == "--repetitions") {
      configuration.repetitions = parse_size(
          require_value(argc, argv, index, argument), argument, false);
      if (configuration.repetitions > maximum_repetition_count) {
        throw std::invalid_argument{"--repetitions exceeds safety limit"};
      }
    } else if (argument == "--warmup") {
      configuration.warmup_repetitions = parse_size(
          require_value(argc, argv, index, argument), argument, true);
      if (configuration.warmup_repetitions > maximum_repetition_count) {
        throw std::invalid_argument{"--warmup exceeds safety limit"};
      }
    } else if (argument == "--workload") {
      configuration.workload = require_value(argc, argv, index, argument);
      if (configuration.workload != "all" &&
          !is_known_workload(configuration.workload)) {
        throw std::invalid_argument{"unknown workload"};
      }
    } else if (argument == "--mode") {
      const std::string_view value = require_value(argc, argv, index, argument);
      if (value == "all") {
        configuration.mode = MeasurementMode::All;
      } else if (value == "throughput") {
        configuration.mode = MeasurementMode::Throughput;
      } else if (value == "latency") {
        configuration.mode = MeasurementMode::Latency;
      } else {
        throw std::invalid_argument{"unknown measurement mode"};
      }
    } else {
      throw std::invalid_argument{"unknown option: " + std::string{argument}};
    }
  }

  const std::size_t total_repetitions =
      configuration.repetitions + configuration.warmup_repetitions;
  if (configuration.event_count >
      maximum_replayed_event_count / total_repetitions) {
    throw std::invalid_argument{
        "events times repetitions and warm-up exceeds safety limit"};
  }

  return configuration;
}

void print_usage(std::ostream &output) {
  output << "Usage: matching_engine_benchmarks [options]\n"
         << "  --events N          events per workload (default 20000)\n"
         << "  --repetitions N     measured repetitions (default 3)\n"
         << "  --warmup N          unmeasured repetitions (default 1)\n"
         << "  --workload NAME     workload name or all\n"
         << "  --mode MODE         all, throughput, or latency\n"
         << "  --list              list workload names\n"
         << "  --help              show this help\n";
}

void print_workload_names(std::ostream &output) {
  for (const std::string_view name : workload_names) {
    output << name << '\n';
  }
}

[[nodiscard]] Event limit_event(OrderId id, Side side, Price price,
                                Quantity quantity = Quantity{1}) {
  return Event{.kind = EventKind::Limit,
               .id = id,
               .side = side,
               .price = price,
               .quantity = quantity};
}

[[nodiscard]] Event market_event(OrderId id, Side side,
                                 Quantity quantity = Quantity{1}) {
  return Event{.kind = EventKind::Market,
               .id = id,
               .side = side,
               .price = Price{},
               .quantity = quantity};
}

[[nodiscard]] Event cancel_event(OrderId id) {
  return Event{.kind = EventKind::Cancel,
               .id = id,
               .side = Side::Buy,
               .price = Price{},
               .quantity = Quantity{}};
}

[[nodiscard]] Workload make_non_marketable_workload(std::size_t event_count) {
  Workload workload{.name = "non_marketable",
                    .description = "non-crossing inserts on both sides",
                    .events = {}};
  workload.events.reserve(event_count);

  for (std::size_t index = 0; index < event_count; ++index) {
    const bool is_buy = index % 2U == 0U;
    const Price offset = static_cast<Price>(index % 10U);
    workload.events.push_back(
        limit_event(static_cast<OrderId>(index) + OrderId{1},
                    is_buy ? Side::Buy : Side::Sell,
                    is_buy ? Price{90} + offset : Price{110} + offset));
  }

  return workload;
}

[[nodiscard]] Workload make_mixed_cancel_workload(std::size_t event_count) {
  Workload workload{.name = "mixed_cancel",
                    .description = "alternating inserts and valid cancels",
                    .events = {}};
  workload.events.reserve(event_count);

  OrderId next_id{1};
  while (workload.events.size() < event_count) {
    const bool is_buy = next_id % OrderId{2} == OrderId{0};
    workload.events.push_back(
        limit_event(next_id, is_buy ? Side::Buy : Side::Sell,
                    is_buy ? Price{95} : Price{105}, Quantity{2}));
    if (workload.events.size() < event_count) {
      workload.events.push_back(cancel_event(next_id));
    }
    ++next_id;
  }

  return workload;
}

[[nodiscard]] Workload
make_highly_marketable_workload(std::size_t event_count) {
  Workload workload{.name = "highly_marketable",
                    .description =
                        "resting liquidity followed by crossing market orders",
                    .events = {}};
  workload.events.reserve(event_count);

  OrderId next_id{1};
  std::size_t pair_index = 0;
  while (workload.events.size() < event_count) {
    const bool market_buy = pair_index % 2U == 0U;
    workload.events.push_back(
        limit_event(next_id, market_buy ? Side::Sell : Side::Buy, Price{100}));
    ++next_id;
    if (workload.events.size() < event_count) {
      workload.events.push_back(
          market_event(next_id, market_buy ? Side::Buy : Side::Sell));
      ++next_id;
    }
    ++pair_index;
  }

  return workload;
}

[[nodiscard]] Workload make_single_price_workload(std::size_t event_count) {
  Workload workload{.name = "single_price",
                    .description = "many FIFO inserts at one bid price",
                    .events = {}};
  workload.events.reserve(event_count);

  for (std::size_t index = 0; index < event_count; ++index) {
    workload.events.push_back(limit_event(
        static_cast<OrderId>(index) + OrderId{1}, Side::Buy, Price{100}));
  }

  return workload;
}

[[nodiscard]] Workload make_deep_book_workload(std::size_t event_count) {
  Workload workload{.name = "deep_book",
                    .description = "one resting bid per distinct price level",
                    .events = {}};
  workload.events.reserve(event_count);

  for (std::size_t index = 0; index < event_count; ++index) {
    workload.events.push_back(
        limit_event(static_cast<OrderId>(index) + OrderId{1}, Side::Buy,
                    Price{1'000'000} + static_cast<Price>(index)));
  }

  return workload;
}

[[nodiscard]] Workload make_workload(std::string_view name,
                                     std::size_t event_count) {
  if (name == "non_marketable") {
    return make_non_marketable_workload(event_count);
  }
  if (name == "mixed_cancel") {
    return make_mixed_cancel_workload(event_count);
  }
  if (name == "highly_marketable") {
    return make_highly_marketable_workload(event_count);
  }
  if (name == "single_price") {
    return make_single_price_workload(event_count);
  }
  if (name == "deep_book") {
    return make_deep_book_workload(event_count);
  }
  throw std::invalid_argument{"unknown workload"};
}

[[nodiscard]] std::uint64_t mix_checksum(std::uint64_t checksum,
                                         std::uint64_t value) {
  return checksum ^ (value + std::uint64_t{0x9E3779B97F4A7C15} +
                     (checksum << 6U) + (checksum >> 2U));
}

[[nodiscard]] std::uint64_t trade_checksum(const Trade &trade) {
  std::uint64_t checksum = trade.aggressor_id;
  checksum = mix_checksum(checksum, trade.resting_id);
  checksum = mix_checksum(checksum, static_cast<std::uint64_t>(trade.price));
  checksum = mix_checksum(checksum, trade.quantity);
  return checksum;
}

[[nodiscard]] EventOutcome submit_outcome(SubmitResult result) {
  std::uint64_t checksum = static_cast<std::uint64_t>(result.error);
  checksum =
      mix_checksum(checksum, static_cast<std::uint64_t>(result.trades.size()));
  for (const Trade &trade : result.trades) {
    checksum = mix_checksum(checksum, trade_checksum(trade));
  }
  return EventOutcome{.error = result.error, .checksum = checksum};
}

[[nodiscard]] EventOutcome execute_event(MatchingEngine &engine,
                                         const Event &event) {
  if (event.kind == EventKind::Limit) {
    return submit_outcome(
        engine.submit_limit(OrderRequest{.id = event.id,
                                         .side = event.side,
                                         .price = event.price,
                                         .quantity = event.quantity}));
  }
  if (event.kind == EventKind::Market) {
    return submit_outcome(engine.submit_market(MarketOrderRequest{
        .id = event.id, .side = event.side, .quantity = event.quantity}));
  }

  const ErrorCode error = engine.cancel(event.id);
  return EventOutcome{.error = error,
                      .checksum = static_cast<std::uint64_t>(error)};
}

void validate_workload(const Workload &workload) {
  MatchingEngine engine;
  for (std::size_t index = 0; index < workload.events.size(); ++index) {
    const EventOutcome outcome = execute_event(engine, workload.events[index]);
    if (outcome.error != ErrorCode::None) {
      throw std::runtime_error{"workload validation failed at event " +
                               std::to_string(index)};
    }

    const auto best_bid = engine.best_bid();
    const auto best_ask = engine.best_ask();
    if (best_bid.has_value() && best_ask.has_value() &&
        *best_bid >= *best_ask) {
      throw std::runtime_error{"workload left a crossed book"};
    }
  }
}

[[nodiscard]] std::uint64_t execute_events(const std::vector<Event> &events) {
  MatchingEngine engine;
  std::uint64_t checksum{};
  for (const Event &event : events) {
    const EventOutcome outcome = execute_event(engine, event);
    checksum = mix_checksum(checksum, outcome.checksum);
  }
  return checksum;
}

void warm_up(const std::vector<Event> &events, std::size_t repetitions) {
  std::uint64_t checksum{};
  for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
    checksum = mix_checksum(checksum, execute_events(events));
  }
  if (checksum == std::numeric_limits<std::uint64_t>::max()) {
    std::cerr << "warm-up checksum=" << checksum << '\n';
  }
}

[[nodiscard]] TimedRun measure_throughput(const std::vector<Event> &events,
                                          std::size_t repetitions) {
  using Clock = std::chrono::steady_clock;

  std::chrono::nanoseconds elapsed{};
  std::uint64_t checksum{};
  for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
    MatchingEngine engine;
    const auto start = Clock::now();
    for (const Event &event : events) {
      const EventOutcome outcome = execute_event(engine, event);
      checksum = mix_checksum(checksum, outcome.checksum);
    }
    const auto finish = Clock::now();
    elapsed +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start);
  }

  return TimedRun{.elapsed = elapsed, .checksum = checksum};
}

void print_throughput_result(const Workload &workload,
                             const Configuration &configuration) {
  warm_up(workload.events, configuration.warmup_repetitions);
  const TimedRun run =
      measure_throughput(workload.events, configuration.repetitions);
  const std::size_t measured_events =
      workload.events.size() * configuration.repetitions;
  const double seconds = std::chrono::duration<double>(run.elapsed).count();
  const double events_per_second =
      seconds > 0.0 ? static_cast<double>(measured_events) / seconds : 0.0;

  std::cout << "result workload=" << workload.name
            << " mode=throughput measured_events=" << measured_events
            << " elapsed_ns=" << run.elapsed.count()
            << " events_per_second=" << std::fixed << std::setprecision(2)
            << events_per_second << " checksum=" << run.checksum << '\n';
}

void print_latency_result(const Workload &workload,
                          const Configuration &configuration) {
  using Clock = std::chrono::steady_clock;

  warm_up(workload.events, configuration.warmup_repetitions);
  std::vector<std::chrono::nanoseconds::rep> samples;
  samples.reserve(workload.events.size() * configuration.repetitions);
  std::uint64_t checksum{};

  for (std::size_t repetition = 0; repetition < configuration.repetitions;
       ++repetition) {
    MatchingEngine engine;
    for (const Event &event : workload.events) {
      const auto start = Clock::now();
      const EventOutcome outcome = execute_event(engine, event);
      const auto finish = Clock::now();
      samples.push_back(
          std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
              .count());
      checksum = mix_checksum(checksum, outcome.checksum);
    }
  }

  std::sort(samples.begin(), samples.end());
  const auto total = std::accumulate(
      samples.begin(), samples.end(), std::uint64_t{0},
      [](std::uint64_t sum, std::chrono::nanoseconds::rep sample) {
        return sum + static_cast<std::uint64_t>(sample);
      });
  const double mean =
      static_cast<double>(total) / static_cast<double>(samples.size());
  const std::size_t middle = samples.size() / 2U;
  const double median = samples.size() % 2U == 0U
                            ? (static_cast<double>(samples[middle - 1U]) +
                               static_cast<double>(samples[middle])) /
                                  2.0
                            : static_cast<double>(samples[middle]);

  std::cout << "result workload=" << workload.name
            << " mode=instrumented_latency samples=" << samples.size()
            << " min_ns=" << samples.front() << " median_ns=" << std::fixed
            << std::setprecision(2) << median << " mean_ns=" << mean
            << " max_ns=" << samples.back() << " checksum=" << checksum << '\n';
}

[[nodiscard]] std::string_view operating_system() {
#if defined(__APPLE__)
  return "macOS";
#elif defined(_WIN32)
  return "Windows";
#elif defined(__linux__)
  return "Linux";
#else
  return "unknown";
#endif
}

[[nodiscard]] std::string_view architecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#else
  return "unknown";
#endif
}

void print_metadata(const Configuration &configuration) {
  std::cout << "benchmark=matching_engine_baseline\n"
            << "compiler_id=" << MATCHING_ENGINE_BENCHMARK_COMPILER_ID << '\n'
            << "compiler_version=" << MATCHING_ENGINE_BENCHMARK_COMPILER_VERSION
            << '\n'
            << "compiler_flags=" << MATCHING_ENGINE_BENCHMARK_FLAGS << '\n'
            << "build_type=" << MATCHING_ENGINE_BENCHMARK_BUILD_TYPE << '\n'
            << "os=" << operating_system() << '\n'
            << "architecture=" << architecture() << '\n'
            << "logical_cpus=" << std::thread::hardware_concurrency() << '\n'
            << "pointer_bits=" << sizeof(void *) * 8U << '\n'
            << "clock=std::chrono::steady_clock\n"
            << "clock_is_steady="
            << (std::chrono::steady_clock::is_steady ? "true" : "false") << '\n'
            << "generator=deterministic_v1\n"
            << "events_per_workload=" << configuration.event_count << '\n'
            << "repetitions=" << configuration.repetitions << '\n'
            << "warmup_repetitions=" << configuration.warmup_repetitions << '\n'
            << "latency_note=per-event clock reads are included\n";
}

void run_workload(std::string_view name, const Configuration &configuration) {
  const Workload workload = make_workload(name, configuration.event_count);
  validate_workload(workload);

  std::cout << "workload=" << workload.name << " description=\""
            << workload.description << "\" events=" << workload.events.size()
            << '\n';

  if (configuration.mode == MeasurementMode::All ||
      configuration.mode == MeasurementMode::Throughput) {
    print_throughput_result(workload, configuration);
  }
  if (configuration.mode == MeasurementMode::All ||
      configuration.mode == MeasurementMode::Latency) {
    print_latency_result(workload, configuration);
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Configuration configuration = parse_configuration(argc, argv);
    if (configuration.show_help) {
      print_usage(std::cout);
      return 0;
    }
    if (configuration.list_workloads) {
      print_workload_names(std::cout);
      return 0;
    }

    print_metadata(configuration);
    if (configuration.workload == "all") {
      for (const std::string_view name : workload_names) {
        run_workload(name, configuration);
      }
    } else {
      run_workload(configuration.workload, configuration);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    print_usage(std::cerr);
    return 2;
  }
}
