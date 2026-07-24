# PLAN.md

## Status

This plan defines the implementation path for an educational C++20 limit order
book and matching engine. Milestone 5 has added a reproducible benchmark
baseline after the depth, snapshot, and randomized invariant work. Measured
optimization remains planned for Milestone 6.

Milestone status values:

- Planned: not started.
- In progress: currently being implemented.
- Complete: implemented, tested, and verified.
- Revised: plan changed with a dated note explaining why.

## Assumptions

- The first implementation is a deterministic, single-threaded matching core.
- The matching core is a library target; command-line tools and benchmarks are
  separate targets.
- Production code uses only the C++ standard library.
- Tests may use GoogleTest if the environment can fetch or provide it cleanly.
- Benchmarks may use Google Benchmark, but benchmark work starts only after the
  correct baseline is implemented.
- Prices are integer ticks. No floating-point prices are used anywhere in core
  domain types.
- Order IDs are unique only among active orders. Reusing an ID after the
  previous order is fully filled or cancelled can be allowed if documented.
- Matching emits deterministic trade events in the order they occur.

## Non-goals

- No networking, FIX protocol, database, GUI, web API, persistence, or trading
  strategy.
- No multi-threaded matching core.
- No lock-free structures, custom allocators, memory pools, or specialized
  hardware assumptions.
- No unsupported latency claims.
- No optimization before a correct and measured baseline exists.

## Proposed repository structure

```text
.
+-- AGENTS.md
+-- PLAN.md
+-- README.md
+-- CMakeLists.txt
+-- cmake/
|   +-- CompilerWarnings.cmake
|   +-- Sanitizers.cmake
+-- include/
|   +-- matching_engine/
|       +-- domain.hpp
|       +-- matching_engine.hpp
|       +-- snapshot.hpp
+-- src/
|   +-- matching_engine.cpp
+-- tests/
|   +-- CMakeLists.txt
|   +-- domain_tests.cpp
|   +-- limit_order_tests.cpp
|   +-- cancel_replace_tests.cpp
|   +-- market_order_tests.cpp
|   +-- invariant_tests.cpp
+-- benchmarks/
|   +-- CMakeLists.txt
|   +-- matching_benchmarks.cpp
+-- tools/
    +-- README.md
```

The exact structure may be simplified in early milestones, but production
domain types, matching logic, tests, and benchmarks should remain separate.

## Public domain model and API draft

The exact names may change during implementation, but the public surface should
stay small and explicit.

```cpp
namespace matching_engine {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;
using SequenceNumber = std::uint64_t;

enum class Side { Buy, Sell };

struct OrderRequest {
  OrderId id;
  Side side;
  Price price;
  Quantity quantity;
};

struct MarketOrderRequest {
  OrderId id;
  Side side;
  Quantity quantity;
};

struct ReplaceRequest {
  OrderId id;
  Price new_price;
  Quantity new_quantity;
};

struct Trade {
  OrderId aggressor_id;
  OrderId resting_id;
  Price price;
  Quantity quantity;
};

enum class ErrorCode {
  None,
  DuplicateOrderId,
  UnknownOrderId,
  InvalidPrice,
  InvalidQuantity
};

struct SubmitResult {
  ErrorCode error;
  std::vector<Trade> trades;
};

class MatchingEngine {
 public:
  SubmitResult submit_limit(OrderRequest request);
  SubmitResult submit_market(MarketOrderRequest request);
  ErrorCode cancel(OrderId id);
  SubmitResult replace(ReplaceRequest request);

  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  std::vector<DepthLevel> depth(Side side, std::size_t levels) const;
  BookSnapshot snapshot() const;
};

}  // namespace matching_engine
```

## Candidate data-structure designs

### Candidate A: ordered maps plus per-level linked lists

- Bids: `std::map<Price, PriceLevel, std::greater<Price>>`.
- Asks: `std::map<Price, PriceLevel, std::less<Price>>`.
- Each `PriceLevel` stores FIFO resting orders in `std::list<Order>`.
- Order index: `std::unordered_map<OrderId, OrderLocation>`.
- `OrderLocation` stores side, price, and the list iterator for arbitrary
  cancellation or replacement.

Benefits:

- Natural best-price lookup with `begin()`.
- FIFO is easy to explain.
- `std::list` iterators remain valid when other orders in the same level are
  inserted or erased.
- Arbitrary cancel by order ID uses an average O(1) index lookup, an O(log P)
  price-level lookup, and an O(1) list erase.

Costs:

- Poorer cache locality than contiguous containers.
- More allocations per order.
- More pointer-heavy than a vector/deque approach.

### Candidate B: ordered maps plus per-level deque/vector

- Bids and asks use ordered maps.
- Each price level stores orders in `std::deque<Order>` or `std::vector<Order>`.
- Order index stores side, price, and an offset or identifier.

Benefits:

- Better locality within a price level.
- Simpler memory layout for append and front matching.

Costs:

- Removing arbitrary middle orders is expensive or leaves tombstones.
- Stored offsets can become stale after erasure.
- Tombstones complicate FIFO, depth, quantity conservation, and invariant
  checks.
- More implementation risk for an educational first version.

### Chosen baseline

Use Candidate A for the baseline: ordered maps for price levels, `std::list` for
FIFO orders within each price level, and `std::unordered_map` for active order
lookup.

This is not the fastest possible design, but it is appropriate because it makes
the correctness properties visible: ordered best prices, FIFO priority,
iterator stability, and safe arbitrary cancellation. It also creates good
interview discussion points about the trade-off between clarity and cache
locality.

## Lifetime, ownership, and iterator analysis

- `MatchingEngine` owns all resting orders through its bid and ask containers.
- The order index does not own orders. It stores locations pointing into the
  owning price-level lists.
- A `std::list<Order>::iterator` remains valid when other elements in the same
  list are inserted or erased.
- A list iterator becomes invalid when its own order is erased.
- A price-level map iterator can be avoided in the order index at baseline; the
  index can store side and price, then look up the price level when needed.
- Erasing an empty price level invalidates only iterators to that price level.
  The order index must have no entries pointing into that level before erasing
  it.
- Trade events are values returned to the caller and do not reference internal
  storage.
- Snapshot and depth results are values copied out of the book.

## Expected complexity

Let:

- `P` be the number of active price levels on one side.
- `N` be the number of active orders.
- `K` be the number of resting orders matched by an incoming order.
- `L` be the requested depth level count.

Expected operation costs:

- Submit non-marketable limit order: O(log P) to find/create the price level,
  average O(1) to index by order ID.
- Submit marketable limit order: expected O(K), plus O(log P) to rest any
  remainder.
- Submit market order: expected O(K). No remainder rests.
- Cancel: average O(1) order lookup, O(log P) price-level lookup, and O(1) list
  erase.
- Replace reducing quantity at same price: average O(1) lookup and O(1)
  mutation.
- Replace changing price or increasing quantity: cancel plus submit semantics.
- Best bid / best ask: O(1) from map `begin()` after checking emptiness.
- Top-N depth: O(min(L, P) + orders visited if aggregation is not maintained).
  A later measured optimization may maintain level aggregate quantity.
- Snapshot: O(N).

Plan correction (2026-07-24): the baseline order index stores side and price,
not a price-level map iterator, so cancellation always performs an O(log P)
price-level lookup. The previous text incorrectly applied that cost only when
an empty level was removed.

Plan correction (2026-07-24): matching erases empty price levels through known
map iterators, which is amortized O(1). The previous O(K log P) estimate
overstated expected limit and market matching costs.

## Matching semantics

- Buy limits match while best ask price is less than or equal to the incoming
  buy price.
- Sell limits match while best bid price is greater than or equal to the
  incoming sell price.
- Market orders match until their quantity is zero or the opposite side is
  empty.
- Trades execute at the resting order's price.
- At the same price, older resting orders fill before newer resting orders.
- If an incoming limit order has remaining quantity after matching, the
  remainder rests on its own side.
- If an incoming market order has remaining quantity after matching, the
  remainder is cancelled.
- Fully filled resting orders are removed from both the price-level list and the
  order index.
- Empty price levels are removed.
- Duplicate active order IDs are rejected before mutating the book.
- Invalid quantity and invalid price are rejected before mutating the book.
- Unknown cancellations and replacements return explicit errors.

## Replacement semantics

- Reducing quantity at the same price preserves time priority.
- Replacing with the same quantity and same price can be treated as a no-op.
- Increasing quantity at the same price loses time priority and is implemented
  as cancel-and-reinsert.
- Changing price loses time priority and is implemented as cancel-and-reinsert.
- Replacement must be exception-safe enough that a failed validation does not
  mutate the original order.
- A replacement that behaves as cancel-and-reinsert may trade immediately if the
  new price crosses the opposite side.

## Core invariants

- No active order has zero remaining quantity.
- Every active order ID appears exactly once in the order index.
- Every order-index entry points to an existing resting order.
- Every resting order appears in the order index.
- No empty price levels remain after matching, cancellation, or replacement.
- Bids are stored in descending price priority.
- Asks are stored in ascending price priority.
- FIFO priority is preserved within each price level.
- The book is not crossed after any public operation completes.
- Total executed quantity never exceeds the incoming quantity or the resting
  quantities available at matching prices.
- Market orders never remain active.
- Trade prices always equal the matched resting order's price.

## Depth and snapshot semantics

- Depth returns at most the requested number of price levels.
- Bid depth is ordered from highest to lowest price; ask depth is ordered from
  lowest to highest price.
- Depth quantity is computed from the resting orders at each price level.
- If a level's aggregate quantity cannot be represented by `Quantity`, depth
  throws `std::overflow_error` instead of returning a wrapped or saturated
  value.
- Snapshot order is deterministic: bids first in descending price order, then
  asks in ascending price order, with FIFO order inside each price level.
- Sequence numbers start at zero and increase when an order rests. Partial
  fills and same-price non-increasing replacements preserve the sequence;
  cancel-and-reinsert replacements receive a new one.

## Test matrix

Domain and validation:

- Construct valid order requests.
- Reject zero quantity.
- Reject invalid limit prices.
- Reject duplicate active order IDs.
- Allow or reject reused inactive order IDs according to documented behavior.

Limit order matching:

- Non-marketable buy rests.
- Non-marketable sell rests.
- Exact-price buy/sell match.
- Buy matches multiple ask price levels.
- Sell matches multiple bid price levels.
- Partial fill leaves incoming remainder resting for limit orders.
- Partial fill leaves resting remainder active.
- Fully filled resting orders are removed.
- Empty price levels are removed.
- Trade price is the resting order's price.
- Best bid and best ask update after each operation.
- Book is not crossed after every operation.

FIFO:

- Multiple buys at same price fill oldest first.
- Multiple sells at same price fill oldest first.
- Partial fill of head order preserves the head order with reduced quantity.

Cancel:

- Cancel head order in a level.
- Cancel middle order in a level.
- Cancel tail order in a level.
- Cancel last order removes the price level.
- Unknown cancel returns `UnknownOrderId`.
- Cancelled order no longer matches or appears in depth/snapshot.

Replace:

- Reduce quantity at same price preserves priority.
- Same price and same quantity is a no-op or documented equivalent.
- Increase quantity at same price loses priority.
- Price change loses priority.
- Unknown replace returns `UnknownOrderId`.
- Invalid replacement quantity is rejected.
- Invalid replacement price is rejected.
- Cancel-and-reinsert style replacement leaves no stale index entry.

Market orders:

- Market buy consumes best asks in FIFO order.
- Market sell consumes best bids in FIFO order.
- Unfilled market remainder is cancelled.
- Market order never appears in the order index, depth, or snapshot.

Depth and snapshot:

- Empty book returns empty best bid/ask and empty depth.
- Top-N depth aggregates quantities by price.
- Top-N depth respects side-specific price ordering.
- Snapshot is deterministic.
- Snapshot contains active orders in predictable side, price, and FIFO order.

Invariant/randomized:

- Deterministic random sequence with recorded seed.
- Compare quantity conservation against a simple reference model where
  practical.
- Check no crossed book after every generated event.
- Check order index and price-level containers remain consistent.

## Benchmark methodology

Benchmarking starts only after correctness tests pass.

Required workloads:

- Mostly non-marketable inserts.
- Mixed inserts and cancellations.
- Highly marketable incoming orders.
- Many orders at one price.
- Deep books with many price levels.

Measurement rules:

- Use Release builds.
- Report compiler, flags, CPU/machine, build type, workload distribution, event
  count, and warm-up.
- Keep correctness validation outside timed hot paths where appropriate.
- Measure throughput separately from instrumented per-event latency.
- Do not report p95 or p99 unless the benchmark records a latency distribution.
- Include benchmark limitations and measurement overhead.
- Do not put benchmark numbers in README until they have been run in the local
  environment.

## Milestones

### Milestone 0: Project rules and plan

Status: Complete.

Scope:

- Maintain the complete `AGENTS.md`.
- Create this `PLAN.md`.
- Do not create production C++ code yet.

Acceptance criteria:

- `AGENTS.md` contains domain, architecture, testing, benchmark, and working
  process rules.
- `PLAN.md` defines assumptions, non-goals, baseline design, milestones, and
  acceptance criteria.
- The repository still contains no production implementation.

### Milestone 1: Build skeleton and domain types

Status: Complete.

Scope:

- Add CMake project configuration.
- Add warning and sanitizer options for project targets.
- Add public headers for domain types.
- Add an empty `MatchingEngine` public interface.
- Add initial tests for domain validation and empty-book queries.
- Add a minimal README with build and test commands.

Acceptance criteria:

- Debug configure succeeds with tests and sanitizers enabled.
- Build succeeds.
- CTest runs and passes.
- Domain types use fixed-width integer aliases.
- Invalid quantities and prices are represented and tested.
- No matching algorithm is implemented in this milestone.

### Milestone 2: Limit order matching core

Status: Complete.

Scope:

- Implement buy and sell limit order submission.
- Implement price-time priority.
- Implement partial fills.
- Implement matching across multiple price levels.
- Rest unfilled limit-order remainders.
- Return deterministic trade events.
- Implement best bid and best ask.

Acceptance criteria:

- Tests cover non-marketable resting orders.
- Tests cover exact-price matches.
- Tests cover multi-level matches.
- Tests cover partial fills on incoming and resting orders.
- Tests prove FIFO behavior at the same price.
- Tests prove trade price equals resting price.
- Tests prove filled orders and empty price levels are removed.
- Tests prove the book is never crossed after submission.
- Debug build, CTest, and sanitizers pass.
- Cancel, replace, market orders, depth, snapshots, parsing, and benchmarks are
  not implemented yet.

### Milestone 3: Cancellation and replacement

Status: Complete.

Scope:

- Implement cancellation by active order ID.
- Implement replacement by active order ID.
- Maintain the order index and price-level containers consistently.
- Apply documented replacement priority rules.

Acceptance criteria:

- Tests cover cancelling head, middle, and tail orders.
- Tests cover cancelling the last order at a price level.
- Tests cover unknown cancellation.
- Tests cover reducing quantity at same price preserving priority.
- Tests cover increasing quantity at same price losing priority.
- Tests cover price change losing priority.
- Tests cover invalid and unknown replacement.
- Tests prove there are no stale order-index entries after cancel or replace.
- Debug build, CTest, and sanitizers pass.

### Milestone 4A: Market orders

Status: Complete.

Scope:

- Implement market buy and market sell orders.
- Ensure unfilled market remainder is cancelled.
- Ensure market orders never rest in the book.

Acceptance criteria:

- Tests cover market buy consuming asks.
- Tests cover market sell consuming bids.
- Tests cover market orders matching multiple levels.
- Tests cover partial market fills with empty opposite side.
- Tests prove market orders do not appear in best price or the order index.
- Debug build, CTest, and sanitizers pass.

Plan correction (2026-07-24): depth and snapshot queries are implemented in
Milestone 4B, so proving that they omit market orders is deferred to that
milestone. Milestone 4A still proves directly that market orders never rest or
enter the active-order index.

### Milestone 4B: Depth, snapshots, and randomized invariants

Status: Complete.

Scope:

- Implement top-N aggregated depth queries.
- Implement deterministic book snapshots for tests and debugging.
- Add deterministic randomized invariant tests.

Acceptance criteria:

- Depth aggregates quantity by price.
- Depth returns side-specific ordering.
- Depth rejects aggregate quantity overflow.
- Empty-book depth and snapshot behavior is tested.
- Snapshot order is deterministic and documented.
- Depth and snapshots do not contain market orders.
- Randomized tests record the seed and operation sequence on failure.
- Invariant tests check index/container consistency, no zero active quantities,
  no empty levels, and no crossed book.
- Debug build, CTest, and sanitizers pass.

### Milestone 5: Benchmark baseline

Status: Complete.

Scope:

- Add a Release benchmark target.
- Add reproducible synthetic workloads.
- Document benchmark methodology and limitations.
- Do not optimize production code in this milestone.

Acceptance criteria:

- Release benchmark build succeeds.
- Workloads include non-marketable inserts, mixed insert/cancel, highly
  marketable orders, single-price stress, and deep-book stress.
- Throughput and instrumented latency are measured separately.
- Benchmark output includes workload settings and environment metadata where
  practical.
- README documents methodology without unsupported performance claims.
- Debug build, CTest, sanitizers, and Release benchmark build pass.

### Milestone 6: One measured optimization

Status: Planned.

Scope:

- Profile or inspect benchmark results from Milestone 5.
- Propose at most two candidate optimizations.
- Implement only one low-risk optimization if measurements justify it.
- Keep correctness and clarity ahead of cleverness.

Acceptance criteria:

- The selected bottleneck is supported by benchmark or profiling evidence.
- The optimization has a written rationale and risk assessment.
- Correctness tests still pass.
- The same benchmark workloads are rerun before and after the change.
- Results report improvements and regressions.
- Unsupported claims are not added.

### Milestone 7: Adversarial review and interview explanation

Status: Planned.

Scope:

- Perform a correctness-focused review of production code, tests, CMake, and
  documentation.
- Fix confirmed issues in separate scoped changes.
- Produce an interview explanation guide after implementation stabilizes.

Acceptance criteria:

- Review checks iterator invalidation, stale index entries, FIFO violations,
  replacement priority, quantity underflow/overflow, crossed books, and
  unsupported documentation claims.
- Confirmed findings have regression tests.
- Final explanation covers data structures, ownership, iterator validity,
  complexity, invariants, rejected alternatives, and measured limitations.

## Risks and likely mistakes

- Storing stale list iterators in the order index after erasing orders.
- Forgetting to remove fully filled orders from the index.
- Forgetting to erase empty price levels.
- Executing trades at the incoming price instead of the resting price.
- Accidentally using price priority correctly but FIFO incorrectly.
- Treating quantity-increasing replacement as priority-preserving.
- Leaving a cancel-and-reinsert replacement in a partially mutated state after
  validation failure.
- Letting a market order rest.
- Leaving the book crossed after a public operation.
- Writing tests that duplicate the implementation instead of checking external
  behavior.
- Reporting benchmark numbers without controlling workload or build type.

## Optimizations explicitly postponed

- Maintaining aggregate quantity per level for faster depth.
- Replacing `std::list` with a more cache-local container.
- Object pools or custom allocators.
- Flat maps, intrusive containers, or tombstone-based storage.
- Multi-threaded ingestion or matching.
- Binary protocols, networking, persistence, or replay engines.

These optimizations may be considered only after the correct baseline and
benchmark methodology are complete.
