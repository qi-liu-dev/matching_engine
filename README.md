# matching_engine

Educational C++20 limit order book and matching engine for software-engineering
interview preparation.

This project prioritizes correctness, determinism, testability, and clear
engineering trade-offs. It is for learning and interview preparation, not for
real-market trading, and does not attempt to clone any proprietary exchange or
trading platform.

## Current status

Milestone 2 is the limit-order matching core:

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
- CTest-based tests

Cancellation, replacement, market orders, depth, snapshots, parsing, and
benchmarks are intentionally deferred to later milestones.

## Build and test

```bash
cmake -S . -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMATCHING_ENGINE_BUILD_TESTS=ON \
  -DMATCHING_ENGINE_ENABLE_SANITIZERS=ON

cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure
```

## Domain conventions

- Prices are integer ticks represented by `matching_engine::Price`.
- A valid limit price is greater than zero.
- Quantities are represented by `matching_engine::Quantity`.
- A valid quantity is greater than zero.
- Floating-point prices are not used in the matching core.
