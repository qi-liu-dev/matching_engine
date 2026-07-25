# Milestone 6 benchmark results

These results justify and evaluate one targeted optimization. They describe one
local machine and are not general performance claims.

These measurements predate the allocation-safety remediation performed after
Milestone 7. They remain historical evidence for selecting the insertion hint,
not current end-to-end throughput measurements.

## Environment

- Date: 2026-07-25
- Baseline source: commit `c4fdad5`
- Compiler: AppleClang 17.0.0.17000603
- Flags: `-O3 -DNDEBUG`
- Build type: Release
- Operating system: macOS
- Architecture: arm64
- Logical CPUs reported by the benchmark: 10
- Clock: `std::chrono::steady_clock`

The baseline and optimized binaries were built separately from the same commit
history and run alternately to reduce time-dependent machine bias. Each row is
the median of three process runs. Every process run used:

```bash
matching_engine_benchmarks \
  --events 100000 \
  --repetitions 7 \
  --warmup 2 \
  --mode throughput
```

## Evidence and selection

The baseline deep-book workload was consistently the slowest insert workload.
It creates 100,000 bid price levels in monotonically improving price order.
Its median throughput was 7.71 million events per second, compared with 27.59
million for inserts at one price. Inspection showed that `rest_order` performed
a general `std::map::try_emplace` lookup for every new best price even though
`begin()` was the known insertion position.

Two candidates were considered:

1. Give `try_emplace` a `begin()` hint only when the book already has multiple
   levels and the resting price is strictly better than the current best.
2. Cache aggregate quantity in each price level to accelerate depth queries.

Candidate 1 was selected because it directly addresses the measured deep-book
workload and changes only the insertion lookup. Candidate 2 was rejected for
this milestone because the baseline workloads do not measure depth queries, so
there was no benchmark evidence to justify its wider invariant surface.

## Results

| Workload | Baseline events/s | Optimized events/s | Change |
| --- | ---: | ---: | ---: |
| `non_marketable` | 24,885,424.84 | 23,917,961.66 | -3.89% |
| `mixed_cancel` | 21,301,119.22 | 21,503,119.01 | +0.95% |
| `highly_marketable` | 15,985,841.11 | 16,008,262.82 | +0.14% |
| `single_price` | 27,587,067.94 | 26,310,637.14 | -4.63% |
| `deep_book` | 7,712,591.98 | 14,841,422.27 | +92.43% |

All before/after workload checksums were identical. The optimization nearly
doubled throughput for the selected deep-book bottleneck on this machine.
It also introduced measurable regressions in the non-marketable and
single-price workloads, so it should not be described as a universal speedup.

## Risk assessment

Correctness risk is low because a map insertion hint is advisory:
`std::map` still determines the actual ordered position. The hint iterator is
borrowed only for the insertion call and is not stored. Existing list iterators
in the order index are unaffected by map insertion.

Performance remains implementation- and workload-dependent. The extra
condition can cost more than it saves in shallow books, as the regressions
above demonstrate. Other standard-library implementations, compilers, CPUs,
price distributions, or longer runs may produce different results.

The harness does not pin threads, isolate cores, or control frequency scaling.
The measurements include normal allocation and checksum costs and should be
rerun before drawing conclusions in another environment.
