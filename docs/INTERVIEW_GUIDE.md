# Matching engine interview guide

This guide explains the implemented system, not an idealized production
exchange. It should be possible to trace every claim here to the code, tests, or
recorded benchmark results.

## Sixty-second summary

The project is a deterministic, single-threaded C++20 limit order book. Bid and
ask prices are stored in ordered maps, and each price level owns a linked list
of resting orders in FIFO order. An unordered map indexes active order IDs to
their list positions.

An incoming order always matches the best eligible opposite price, then the
oldest order at that price. Trades execute at the resting order's price. Limit
remainders rest; market remainders are cancelled. Cancellation and replacement
use the active-order index rather than scanning the book.

The baseline favors visible correctness and stable iterators over cache
locality. A measured insertion hint is the only production optimization.

## Scope

Implemented:

- limit and market orders
- price-time priority and partial fills
- cancellation and replacement
- best bid and ask
- aggregated top-N depth
- deterministic snapshots
- deterministic trades and randomized invariant tests
- synthetic throughput and instrumented-latency benchmarks

Intentionally excluded:

- networking, FIX, persistence, and external market-data parsing
- concurrency and lock-free structures
- custom allocators and memory pools
- trading strategies, risk checks, and account state
- floating-point prices

## Domain model

`OrderId`, `Quantity`, and `SequenceNumber` are unsigned 64-bit integers.
`Price` is a signed 64-bit integer representing ticks.

Valid quantities are greater than zero. Valid limit prices are greater than
zero. Order IDs are unique among active orders, so an ID may be reused after
its previous order is filled or cancelled.

`Side` has two supported values: `Buy` and `Sell`. The API assumes callers pass
one of those enumerators; it does not define behavior for an artificially
constructed out-of-range enum value.

## Matching semantics

For a buy limit:

1. Reject an invalid request or duplicate active ID without mutation.
2. Match while the best ask is less than or equal to the limit price.
3. At each price, consume the FIFO list from the front.
4. Emit each trade at the resting ask's price.
5. Rest any incoming remainder as a bid.

A sell limit is symmetric: it matches bids greater than or equal to its limit.

A market order performs the same best-price/FIFO matching without a price
condition. Any unfilled remainder disappears and never enters the order index.

## Replacement semantics

A same-price quantity reduction preserves priority because it mutates the
existing list node in place. Same price and same quantity is an equivalent
no-op.

A quantity increase or price change loses priority and behaves as
cancel-and-reinsert. Before mutation, the engine plans eligible matches,
reserves the trade result, and creates any required destination price level. A
resting remainder reuses the original list node and is spliced to the back of
its destination level. The replacement can trade immediately and receives a new
sequence number only if a remainder rests.

Validation happens before any replacement mutation, so an invalid replacement
leaves the original order unchanged.

## Data structures

| Structure | Purpose |
| --- | --- |
| `std::map<Price, PriceLevel, std::greater<Price>>` | Bids, best price at `begin()` |
| `std::map<Price, PriceLevel, std::less<Price>>` | Asks, best price at `begin()` |
| `std::list<RestingOrder>` | Stable FIFO order within a price level |
| `std::unordered_map<OrderId, OrderLocation>` | Average O(1) active-ID lookup |

`OrderLocation` stores the side, price, and list iterator. It deliberately does
not store a price-level map iterator, so cancel performs a map lookup by price.

The `MatchingEngine` is neither copyable nor movable. Its index contains
iterators into its own containers, and deleting these operations avoids
requiring subtle iterator-rebinding arguments.

## Ownership and iterator validity

The engine owns both price maps. Each map node owns one `PriceLevel`, and each
level owns its resting orders through a list. The active-order index owns no
orders.

Key iterator rules:

- Inserting into a list does not invalidate iterators to existing nodes.
- Erasing one list node invalidates only the iterator to that node.
- Inserting into a map does not invalidate existing map iterators.
- Erasing a map level invalidates iterators only to that level.
- The Milestone 6 map hint is borrowed for one insertion call and is not stored.

When a resting order is fully filled, its index entry is erased before its list
node. When the resulting list is empty, the map level is erased last. Cancel
uses the same ownership order.

Trades, depth levels, and snapshots are returned as independent values. They do
not contain references or iterators into the engine.

## Core invariants

After every successful public operation:

- each active ID appears exactly once in the index
- each index entry identifies one existing resting order
- each resting order appears in the index
- no active order has zero quantity
- no empty price level remains
- bids are descending and asks are ascending
- FIFO order is preserved within each level
- the best bid is strictly below the best ask when both exist
- market orders are never active
- trade prices equal resting prices
- total execution cannot exceed incoming or resting quantity

The deterministic snapshot and randomized reference-model test make most of
these invariants observable without exposing internal containers.

## Complexity

Let `P` be active price levels on one side, `K` matched resting orders, `N`
active orders, and `L` requested depth levels.

| Operation | Expected cost |
| --- | --- |
| Non-marketable limit insert | O(log P), plus average O(1) ID indexing |
| Strictly new best insert with measured hint | Amortized O(1) level insertion |
| Marketable limit | Expected O(K), plus O(log P) if a remainder rests |
| Market order | Expected O(K) |
| Cancel | Average O(1) ID lookup, O(log P) level lookup, O(1) list erase |
| Same-price quantity reduction | Average O(1) lookup and O(1) mutation |
| Price change or quantity increase | Expected O(K + log P), including match planning |
| Best bid or ask | O(1) |
| Top-N depth | O(min(L, P) plus orders visited in those levels) |
| Snapshot | O(N) |

Space is O(N + P). The index and list each hold information for every active
order, while each active price contributes one map node.

## Why these containers

Ordered maps make best-price selection and side-specific ordering explicit.
Lists make FIFO insertion, front matching, and arbitrary indexed cancellation
straightforward without invalidating unrelated order iterators.

The main cost is pointer-heavy allocation and weaker cache locality.

Rejected baseline alternatives:

- `vector` or `deque` levels: better locality, but arbitrary cancellation
  shifts elements, invalidates positions, or requires tombstones.
- Cached aggregate depth: faster queries, but every fill, cancel, and replace
  must maintain another quantity invariant. It was not justified by the
  measured workloads.
- Stored price-level iterators in the index: could reduce cancel lookup cost,
  but increases iterator-lifetime coupling.
- Pools, custom allocators, intrusive containers, and flat maps: postponed
  until measurements justify their additional complexity.

## Measured optimization

The deep-book benchmark inserts monotonically improving bid prices and was the
slowest insertion workload in the baseline. Once multiple levels exist, a
strictly better resting price has a known map position immediately before
`begin()`. Supplying that hint makes the insertion amortized O(1) when the
standard-library implementation accepts the hint as correct.

Local median results showed a large deep-book improvement but regressions in
some shallow-book workloads. The optimization is therefore targeted, not a
universal speed claim. Exact environment, numbers, and limitations are in
`benchmarks/RESULTS.md`.

## Adversarial review

The Milestone 7 review and follow-up remediation checked:

- full and partial fill erasure order
- cancellation of head, middle, tail, and final level orders
- cancel-and-reinsert replacement priority
- same-price reduction priority
- duplicate and unknown ID failures
- maximum-quantity subtraction and unfilled market remainders
- checked depth aggregation overflow
- crossed-book prevention after every matching path
- snapshot/index consistency under deterministic randomized events
- project-only warning and sanitizer flags
- allocation-failure rollback and operation boundaries
- benchmark timing boundaries and unsupported documentation claims

The follow-up review confirmed that allocation failure could leave an empty
level or an unindexed order, discard an original replacement, or apply earlier
fills without returning their trades. Resting insertion now rolls back partial
container changes. Matching reserves trade results before mutation, and
replacement allocates before reusing or removing its original list node.

The latency timer now ends when the engine API returns, before trade-result
checksumming. Benchmark identity metadata no longer labels optimized builds as
the baseline. The randomized reference model independently selects the expected
best-price/FIFO resting order instead of trusting emitted resting IDs.

## Residual limitations

The engine returns domain validation errors but does not translate allocation
failures into `ErrorCode`. Potentially throwing result and container allocations
are completed before matching mutates resting liquidity, and resting-order
insertion rolls back its map, list, and index changes before propagating
`std::bad_alloc`.

Sequence-number wraparound is not handled. Reaching it would require
`2^64` successful rests in one engine instance, but the limitation is still
explicit.

The implementation assumes valid `Side` enumerators and single-threaded access.
It has no internal synchronization.

Benchmark results depend on the compiler, standard library, allocator, CPU,
frequency state, and workload. Instrumented latency is perturbed by clock reads,
and the harness does not pin or isolate a CPU.

## Interview questions

### Why is the order index not the owner?

The price-level lists define price-time ordering and naturally own the order
nodes. The index is an acceleration structure. Giving both structures ownership
would create duplicate lifetime authority and make erasure harder to reason
about.

### Why erase the index entry before the list node?

The index contains an iterator to that node. Erasing the index first ensures no
stored iterator remains after the node is destroyed.

### Why does a quantity increase lose priority?

Allowing an order to add quantity while retaining its old position would let it
jump ahead of orders that arrived earlier for that additional quantity.
Cancel-and-reinsert gives the changed order a new position.

### Why is the trade price the resting price?

The resting order supplied liquidity at a previously accepted price. The
incoming aggressor accepts that price when it crosses the book.

### Why not use floating-point prices?

Binary floating point cannot exactly represent many decimal prices. Integer
ticks provide exact comparison, deterministic map keys, and simpler validation.

### What would you optimize next?

First add a workload that measures the suspected bottleneck. Candidates might
include cached level aggregates for depth or a more cache-local order
container, but either change expands the invariant surface and needs
before/after evidence.
