# matching_engine

Educational C++20 limit order book and matching engine for software-engineering
interview preparation.

This project prioritizes correctness, determinism, testability, and clear
engineering trade-offs. It is for learning and interview preparation, not for
real-market trading, and does not attempt to clone any proprietary exchange or
trading platform.

## Current status

Milestone 7 completes the adversarial review and interview explanation:

- CMake project configuration
- project-only compiler warnings
- optional AddressSanitizer and UndefinedBehaviorSanitizer flags
- fixed-width domain type aliases
- validation helpers for prices and quantities
- limit buy and sell submission
- price-time priority
- partial fills
- deterministic trade events
- best bid and best ask queries
- cancellation by active order ID
- same-price quantity reductions that preserve priority
- quantity increases and price changes that lose priority
- price-changing replacements that may match immediately
- explicit errors for unknown cancellations and replacements
- market buys and sells with price-time priority
- cancellation of unfilled market-order remainders
- duplicate active ID rejection for market orders
- top-N depth aggregated by price
- deterministic bids-first book snapshots
- resting-order sequence numbers
- checked depth aggregation that rejects quantity overflow
- fixed-seed randomized invariant tests
- five deterministic synthetic benchmark workloads
- separate throughput and instrumented per-event latency measurements
- benchmark environment and workload metadata
- a measured new-best price-level insertion hint with documented trade-offs
- reviewed iterator, index, FIFO, replacement, arithmetic, and crossed-book
  invariants
- an interview guide covering design decisions and residual limitations
- CTest-based tests

External command and market-data parsing are outside the current roadmap and
would require a separate future plan.

## Build and test

```bash
cmake -S . -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMATCHING_ENGINE_BUILD_TESTS=ON \
  -DMATCHING_ENGINE_ENABLE_SANITIZERS=ON

cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure
```

## Benchmark

Configure and build the Release benchmark target separately:

```bash
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMATCHING_ENGINE_BUILD_BENCHMARKS=ON

cmake --build build/release -j
./build/release/benchmarks/matching_engine_benchmarks
```

The benchmark is a small C++20 `std::chrono::steady_clock` harness and has no
additional production dependency. Its deterministic workloads are:

- `non_marketable`: alternating non-crossing bid and ask inserts.
- `mixed_cancel`: alternating resting inserts and valid cancellations.
- `highly_marketable`: resting liquidity followed by opposite-side market
  orders.
- `single_price`: FIFO inserts concentrated at one bid price.
- `deep_book`: one resting bid at each of many distinct price levels.

Use `--help` for all options. For example, this runs one workload with explicit
settings:

```bash
./build/release/benchmarks/matching_engine_benchmarks \
  --workload mixed_cancel \
  --events 100000 \
  --repetitions 5 \
  --warmup 2 \
  --mode all
```

Each workload is generated and validated before timing. Throughput uses one
clock interval around a complete event batch and reports events per second.
Instrumented latency records a clock interval for every event and reports the
observed minimum, median, mean, and maximum. Each repetition starts with a new
empty engine. Throughput includes event execution and result checksumming.
Per-event latency times event execution but excludes sample storage and
trade-result checksumming. Workload generation, validation, engine construction,
sample sorting, and output are excluded from both modes.

Output records the compiler, compiler version and flags, build type, operating
system, architecture, logical CPU count, event count, repetitions, warm-up, and
workload name. Compare results only when these settings and the machine
environment are controlled.

The latency mode uses two clock reads per event, so observation perturbs the
operation even though sample storage is outside the timed interval. It measures
instrumented latency rather than an unobserved production latency distribution.
The harness does not pin threads, disable frequency scaling, isolate CPU cores,
or report tail percentiles. Short runs can be dominated by timer resolution and
operating-system noise. The README therefore contains no performance claims or
stored benchmark numbers.

The local Milestone 6 before/after measurements, candidate selection, and risk
assessment are recorded in
[`benchmarks/RESULTS.md`](benchmarks/RESULTS.md).

The architecture, ownership model, matching flows, complexity, invariants, and
review findings are explained in
[`docs/INTERVIEW_GUIDE.md`](docs/INTERVIEW_GUIDE.md).

## Domain conventions

- Prices are integer ticks represented by `matching_engine::Price`.
- A valid limit price is greater than zero.
- Quantities are represented by `matching_engine::Quantity`.
- A valid quantity is greater than zero.
- Floating-point prices are not used in the matching core.
