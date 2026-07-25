# C++ Limit Order Book and Matching Engine

A deterministic, in-memory C++20 implementation of a limit order book and
matching engine.

The engine accepts buy and sell orders, matches executable orders according to
price-time priority, generates trade events, and maintains the remaining market
liquidity. The implementation focuses on correctness, predictable behaviour,
and explicit data-structure and performance trade-offs.

## The problem

An electronic market continuously receives orders from participants who want to
buy or sell an asset. Each order contains:

- a unique order ID;
- a side: buy or sell;
- a quantity;
- and, for a limit order, a maximum buying price or minimum selling price.

The matching engine must process each incoming order and determine:

1. whether it can trade against an order already resting in the book;
2. which resting order has priority;
3. the execution price and quantity;
4. whether either order is fully or partially filled;
5. and whether any unmatched quantity should remain active.

After every operation, the engine must keep the order book, price levels, and
order-ID index consistent.

## What this project does

The project provides the core lifecycle of an exchange-style order book:

```text
Order submission
       |
       v
Input validation
       |
       v
Price-time-priority matching
       |
       +---------------------> Trade events
       |
       v
Resting-order insertion, update, or removal
       |
       v
Updated best prices and market depth
```

It currently supports:

- limit buy and sell orders;
- market buy and sell orders;
- cancellation by active order ID;
- price and quantity replacement;
- price-time-priority matching;
- partial fills and matching across multiple price levels;
- deterministic trade events;
- best-bid and best-ask queries;
- aggregated top-N market depth;
- deterministic order-book snapshots;
- fixed-seed randomized correctness tests;
- deterministic benchmark workloads.

## Example

Assume the current sell side contains:

```text
ASK  50 @ 100.20
ASK  80 @ 100.25
```

An incoming limit buy order arrives:

```text
BUY 100 @ 100.25
```

The order is executable against both resting sell prices. The engine produces:

```text
TRADE 50 @ 100.20
TRADE 50 @ 100.25
```

The incoming buy order is fully filled. The second sell order remains active
with a quantity of 30:

```text
ASK 30 @ 100.25
```

## Matching rules

### Price priority

Buy orders with higher prices have priority:

```text
BUY 100.20
BUY 100.10
BUY 100.00
```

Sell orders with lower prices have priority:

```text
SELL 100.10
SELL 100.20
SELL 100.30
```

### Time priority

Orders at the same price are matched in arrival order.

```text
Order A: BUY 50 @ 100.10
Order B: BUY 80 @ 100.10
```

Order A executes before Order B because it entered the book first.

### Execution price

A trade executes at the price of the resting order already present in the
book.

### Partial fills

An incoming order may match one or more resting orders and may continue across
multiple price levels.

If a resting order is only partially filled, its remaining quantity stays in
its existing position. If an incoming limit order is partially filled, its
remaining quantity rests in the book. An unfilled market-order remainder is
cancelled and never rests.

### Replacement priority

Replacing an active order follows these rules:

- reducing quantity at the same price preserves time priority;
- increasing quantity loses time priority;
- changing price loses time priority;
- a price-changing replacement may immediately match against the opposite side.

A replacement that loses priority is processed as removal followed by a new
submission under the same order ID.

## Supported operations

### Submit a limit order

A limit order specifies the worst acceptable execution price.

- A buy limit order may execute against sell orders at or below its limit.
- A sell limit order may execute against buy orders at or above its limit.
- Any unmatched quantity remains active in the order book.

### Submit a market order

A market order executes against the best available resting liquidity without a
limit price. Matching continues until the requested quantity is filled or the
opposite side has no remaining liquidity.

Any unfilled quantity is cancelled.

### Cancel an order

An active order can be removed using its order ID. The engine maintains a direct
order-ID index, so cancellation does not require scanning the complete order
book.

### Replace an order

An active order's price or quantity can be changed. The engine applies the
replacement-priority rules above and may generate trades when the replacement
crosses the opposite side.

### Query the book

The engine exposes the current market state through:

- best bid;
- best ask;
- aggregated top-N bid depth;
- aggregated top-N ask depth;
- deterministic full-book snapshots.

## Order-book representation

The book has two independently ordered sides:

```text
Bids: active buy orders, highest price first
Asks: active sell orders, lowest price first
```

Each side is organised into price levels. Orders within a price level are kept
in FIFO order to preserve time priority.

A separate order-ID index points directly to active orders so that cancellation
and replacement can locate an order without traversing all price levels.

```text
                         Matching Engine
                                |
                +---------------+---------------+
                |                               |
              Bids                            Asks
       highest price first             lowest price first
                |                               |
          Price levels                      Price levels
                |                               |
           FIFO orders                       FIFO orders

                       Active order-ID index
                                |
                    direct order-location lookup
```

The design separates three concerns:

1. ordered price discovery;
2. FIFO priority within a price level;
3. direct lookup by order ID.

## Domain conventions

The matching core uses fixed-width integer types for its main values:

```cpp
OrderId
Price
Quantity
SequenceNumber
```

The following rules apply:

- prices are represented as integer ticks, not floating-point values;
- a valid limit price is greater than zero;
- a valid quantity is greater than zero;
- active order IDs must be unique;
- market and limit orders share the same active order-ID namespace;
- sequence numbers represent resting-order time priority.

For example, an application may interpret:

```text
10020 ticks = 100.20
```

The conversion between ticks and a displayed currency value belongs outside the
matching core.

## Core invariants

The implementation maintains the following properties:

- active order IDs are unique;
- no active order has zero remaining quantity;
- orders at the same price retain FIFO priority;
- filled and cancelled orders are removed from every internal index;
- empty price levels are removed;
- the order-ID index and price-level containers describe the same active set;
- processed books do not remain crossed;
- generated trade quantities do not exceed either participating order;
- incoming quantity is conserved across fills and any resting remainder;
- deterministic input produces deterministic trades and final state.

## Correctness and safety

### Integer arithmetic

Floating-point values are not used for matching decisions. Prices and
quantities use integer representations, and aggregated market depth is checked
for overflow.

### Ownership and iterator stability

The engine uses explicit ownership and stable order locations for active-order
lookup. Filled, cancelled, and replaced orders are removed without leaving
stale index entries.

### Exception-safe state changes

Operations that may allocate memory are structured so that allocation failure
does not leave the order book partially updated. This includes resting-order
insertion, submissions that fill multiple orders, and replacements that remove
and reinsert liquidity.

### Deterministic processing

The matching core processes one command at a time. Given the same initial state
and command sequence, it produces the same trade sequence and final order-book
snapshot.

## Testing

The CTest-based test suite covers:

- non-marketable limit orders resting in the book;
- exact-price matching;
- matching across multiple price levels;
- partial fills of incoming and resting orders;
- FIFO priority at one price;
- removal of fully filled orders;
- removal of empty price levels;
- cancellation from the head, middle, and tail of a price level;
- duplicate and unknown order IDs;
- same-price and price-changing replacements;
- replacement priority rules;
- market-order execution and unfilled remainders;
- best-price and empty-book queries;
- top-N aggregated depth;
- deterministic snapshots;
- quantity conservation;
- order-index consistency;
- arithmetic overflow handling;
- crossed-book prevention;
- allocation-failure safety.

Fixed-seed randomized tests compare the implementation with an independent
price-time-priority reference model. Recording the seed makes any failing
sequence reproducible.

## Requirements

- C++20-compatible compiler;
- CMake 3.20 or newer.

Optional development tools include AddressSanitizer and
UndefinedBehaviorSanitizer when supported by the compiler and platform.

## Build and test

Configure a Debug build with tests and sanitizers:

```bash
cmake -S . -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMATCHING_ENGINE_BUILD_TESTS=ON \
  -DMATCHING_ENGINE_ENABLE_SANITIZERS=ON
```

Build the project:

```bash
cmake --build build/debug -j
```

Run all tests:

```bash
ctest --test-dir build/debug --output-on-failure
```

To build without sanitizers, set:

```bash
-DMATCHING_ENGINE_ENABLE_SANITIZERS=OFF
```

## Benchmarking

The repository contains a small C++20 benchmark harness based on
`std::chrono::steady_clock`. It uses deterministic synthetic order flows and
adds no dependency to the production matching core.

### Build the benchmark target

Configure a separate Release build:

```bash
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMATCHING_ENGINE_BUILD_BENCHMARKS=ON
```

Build and run:

```bash
cmake --build build/release -j
./build/release/benchmarks/matching_engine_benchmarks
```

Use `--help` to list all supported options:

```bash
./build/release/benchmarks/matching_engine_benchmarks --help
```

Example:

```bash
./build/release/benchmarks/matching_engine_benchmarks \
  --workload mixed_cancel \
  --events 100000 \
  --repetitions 5 \
  --warmup 2 \
  --mode all
```

### Workloads

The benchmark includes five deterministic workloads:

- `non_marketable`: alternating non-crossing bid and ask insertions;
- `mixed_cancel`: alternating resting insertions and valid cancellations;
- `highly_marketable`: resting liquidity followed by opposite-side market
  orders;
- `single_price`: FIFO insertions concentrated at one bid price;
- `deep_book`: one resting bid at each of many distinct price levels.

### Measurement modes

Throughput mode places one clock interval around a complete event batch and
reports processed events per second. It includes event execution and result
checksumming.

Instrumented latency mode records one clock interval for every event and
reports the observed minimum, median, mean, and maximum. Event execution is
inside the timed interval; sample storage and trade-result checksumming are
outside it.

Each repetition starts with a new empty engine. Workload generation,
validation, engine construction, sample sorting, and console output are
excluded from both measurement modes.

### Reproducibility

Benchmark output records:

- compiler and compiler version;
- compiler flags;
- build type;
- operating system and architecture;
- logical CPU count;
- workload name;
- event count;
- repetition count;
- warm-up count.

Results should only be compared when the workload settings and machine
environment are controlled.

Instrumenting each event requires two clock reads and therefore perturbs the
operation being measured. The harness also does not pin threads, isolate CPU
cores, disable frequency scaling, or claim an unobserved production latency
distribution. For these reasons, performance numbers are not embedded in this
README.

## Scope

This repository implements the in-memory matching core and its correctness and
performance harnesses.

It intentionally does not include:

- connectivity to a live exchange;
- FIX or other exchange protocols;
- external command or market-data parsing;
- user authentication;
- account and balance management;
- pre-trade risk checks;
- persistent order storage;
- clearing and settlement;
- trading strategies;
- profit-and-loss calculations;
- a web or graphical trading interface;
- distributed matching;
- lock-free queues or custom allocators.

These components may exist around a matching engine in a broader trading
platform, but they are separate from the order-matching problem addressed by
this project.
