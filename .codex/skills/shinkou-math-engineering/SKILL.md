---
name: shinkou-math-engineering
description: Evolve the Shinkou Engine math stack for robust, high-performance non-physics engine workloads, including transforms, geometry, SIMD, parallel batches, spatial queries, and advanced numerical algorithms.
metadata:
  short-description: Shinkou high-performance math workflow
---

# Shinkou Math Engineering

Use this skill when changing `engine/include/shinkou/Math*.h`, `engine/src/math`, math consumers, or math tests/benchmarks.

## Scope

- Treat PhysX integration and general-purpose physics simulation as out of scope unless explicitly requested.
- Prioritize renderer, animation, editor picking, culling, spatial queries, and batch CPU workloads.
- Prefer a small, stable public API with explicit scalar/SIMD/batch layers over broad template abstractions.
- Use an external library only when it provides a clear capability or performance benefit that would be costly or error-prone to maintain locally; keep the dependency optional and isolated.

## Required workflow

1. Inspect the current data layout, matrix convention, invalid-input policy, compiler flags, consumers, and existing tests before editing.
2. Preserve column-major `Mat4`, the engine's coordinate/depth convention, trivially-copyable hot-path types, and ABI-sensitive sizes unless the task explicitly authorizes a migration.
3. Separate scalar correctness from batch performance. Every new operation needs scalar tests; SIMD and parallel variants must be checked against the scalar reference, including tails, empty input, in-place input/output where supported, aliasing assumptions, non-finite input, and degenerate geometry.
4. Use scale-aware tolerances for numerical predicates. Avoid using one absolute epsilon for all coordinate magnitudes; document any intentional tolerance.
5. Keep hot loops allocation-free. Favor contiguous spans/views, SoA or structure-of-arrays batch paths when they materially improve throughput, and reuse scratch storage.
6. Add or update a benchmark for operations on realistic batch sizes. Report correctness and throughput separately; do not infer performance from unit-test duration.
7. Run focused math tests, focused benchmarks, then the relevant engine test set and both available build configurations when practical.
8. Summarize numerical limits, dependency changes, benchmark conditions, and any remaining unsupported edge cases.

## Algorithm-library routing

Implement engine-facing algorithms locally when they are small, deterministic, and tightly coupled to the engine's layouts. Consider Eigen, xsimd, or another mature library only for substantial linear algebra, vectorized kernels, or decomposition/solver breadth; do not introduce a dependency merely to replace a few dozen lines of stable code.

For advanced additions, prefer this order:

- robust scalar reference and contracts;
- data-oriented batch API;
- SIMD backend with a portable fallback;
- parallel scheduling around coarse batches;
- optional external backend behind a narrow adapter if benchmarks justify it.

## Deliverables

- public headers and implementation changes;
- focused correctness and adversarial tests;
- a repeatable benchmark target or script;
- concise workflow/design documentation when the capability is architectural.

