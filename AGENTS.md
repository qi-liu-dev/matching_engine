# AGENTS.md

## Project purpose

This repository contains an educational, exchange-style C++ limit order book
and matching engine for software-engineering interviews.

It is not a clone of Optiver, IMC, or any proprietary trading platform.
Prefer correctness, determinism, testability, and clear engineering trade-offs
over impressive-sounding but unsupported performance claims.

The owner of this repository is learning C++ and must be able to understand
and explain every important design decision.

## Language and build

- Use C++20.
- Use CMake 3.20 or newer.
- Use only the C++ standard library in production code unless a dependency is
  explicitly approved.
- GoogleTest may be used for tests.
- Google Benchmark may be used for benchmarks.
- Format C++ code with clang-format.
- Enable useful compiler warnings for project targets.
- Do not apply project warning flags to third-party dependencies.

Expected verification commands:

```bash
cmake -S . -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMATCHING_ENGINE_BUILD_TESTS=ON \
  -DMATCHING_ENGINE_ENABLE_SANITIZERS=ON

cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure
```

Release benchmark build:

```bash
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMATCHING_ENGINE_BUILD_BENCHMARKS=ON

cmake --build build/release -j
```

## Domain rules

- Represent prices as integer ticks, never floating-point values.
- Use fixed-width integer types for order IDs, prices, quantities, and sequence
  numbers.
- Reject zero quantities and invalid limit prices.
- Preserve price-time priority.
- Execute trades at the resting order's price.
- Support partial fills.
- A quantity-reducing replacement at the same price preserves time priority.
- A price change or quantity increase loses time priority and is treated as
  cancel-and-reinsert.
- Reject duplicate active order IDs.
- Unknown cancellations and replacements must return explicit errors.
- Market orders never rest in the book.
- After processing an incoming order, the book must not remain crossed.
- Document any domain behavior that is intentionally simplified.

## Architecture rules

- Keep the matching core deterministic and single-threaded.
- Separate domain types, matching logic, input/output adapters, tests, and
  benchmarks.
- Maintain ordered bid and ask price levels.
- Preserve FIFO order within each price level.
- Provide average O(1) lookup of active orders by order ID.
- Use RAII.
- Do not use owning raw pointers.
- Avoid global mutable state.
- Avoid unnecessary inheritance and generic abstractions.
- Prefer readable value types and explicit interfaces.
- Comments should explain non-obvious invariants or trade-offs, not restate
  code.

## Do not do these without explicit approval

- Do not introduce lock-free data structures.
- Do not implement a custom allocator or memory pool.
- Do not add networking, FIX, databases, web APIs, or a GUI.
- Do not add a trading strategy or P&L simulation.
- Do not call the project "production-grade", "ultra-low-latency", or an "HFT
  platform".
- Do not fabricate benchmark results.
- Do not optimize before establishing a correct baseline and measuring it.
- Do not silently skip failing tests, warnings, or sanitizers.
- Do not change documented matching semantics merely to simplify an
  implementation.

## Testing requirements

Every behavior change must include tests.

Tests should cover:

- non-marketable orders resting in the book
- exact-price matches
- matching across multiple price levels
- partial fills on both incoming and resting orders
- FIFO priority at the same price
- full removal of filled orders and empty price levels
- cancellation of head, middle, and tail orders
- unknown and duplicate order IDs
- replacement priority rules
- empty-book queries
- quantity conservation
- no zero-quantity active orders
- no crossed book after processing
- consistency between the order index and the price-level containers

Randomized tests must use a recorded deterministic seed so failures can be
reproduced.

## Benchmark requirements

- Benchmarks must use a Release build.
- Report the compiler, flags, machine, workload, event count, and warm-up.
- Keep correctness checks separate from the timed hot path where appropriate.
- Measure several workloads rather than one favorable workload.
- Distinguish throughput benchmarks from instrumented per-event latency.
- Do not claim p95 or p99 latency unless the benchmark explicitly records a
  latency distribution.
- Explain measurement overhead and limitations.
- Never place unverified benchmark numbers in README or resume text.

## Working process

Before making changes:

1. Inspect the current repository and relevant documentation.
2. Read PLAN.md if it exists.
3. State any material assumptions.
4. Keep the change scoped to the requested milestone.

After making changes:

1. Format changed files.
2. Build the relevant targets.
3. Run the relevant tests.
4. Run sanitizers for changes affecting ownership or iterators.
5. Review the diff for correctness and unnecessary complexity.
6. Update PLAN.md without rewriting completed history.
7. Summarize:
   - files changed
   - design decisions
   - commands run and their results
   - remaining risks or limitations

Do not start a later milestone unless the user explicitly requests it.
