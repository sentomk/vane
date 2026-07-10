# Architecture (V1 draft)

vane accelerates C++ cold builds by sharing the *live frontend execution*
of the common prefix across many translation units, rather than the
serialized artifacts other approaches share.

## The problem

Traditional compilation:

```
P + A
P + B
P + C
```

Where `P` is the frontend work over the shared header closure and `A/B/C`
is per-TU work. In large projects, `P` runs identically thousands of
times.

## The reshape

```
        P
      / | \
     A  B  C
```

`P` runs once as a live `CompilerInstance`. At a well-defined checkpoint,
the process `fork()`s into N children. Each child continues with its own
remaining source and emits its own `.o`. Copy-on-write makes the shared
frontend state effectively free to hand out.

## What's shared, what isn't

| Approach       | Shares                    | Boundary                 |
|----------------|---------------------------|--------------------------|
| PCH            | Serialized AST bytes      | Declared statically      |
| C++20 Modules  | Serialized BMI            | Declared in source       |
| ccache / cHash | Final `.o`                | Per-TU, after the fact   |
| clang-cache    | Intermediate CAS records  | Internal, per-computation|
| **vane**       | **Live `CompilerInstance`** | **Discovered dynamically**|

vane's sharing unit is orthogonal to the caching family. Both can coexist.

## Components

```
compile_commands.json
        │
        ▼
    Planner                    normalize flags, discover shared
        │                      prefix per flag-equivalence class
Prefix Tree
        │
        ▼
Execution Engine               drive CompilerInstance to checkpoint,
        │                      fork per-TU, collect .o
共享 Prefix
        │
    Checkpoint
      /  |  \
   TU1 TU2 TU3
```

### Planner

- Reads `compile_commands.json`.
- Normalizes compile args and partitions TUs into flag-equivalence classes
  (different `-D`, `-I`, `-std`, `-f*` cannot share a prefix).
- Preprocess-scans each TU to find the longest common prefix within a
  class, building a Prefix Tree of nested shared regions.

### Execution Engine

- Owns the `CompilerInstance` lifecycle.
- Drives frontend execution to the prefix checkpoint.
- Calls `fork()` per child TU (via a bounded pool to control memory).
- Post-fork, the child switches the preprocessor to its remaining source
  and runs parse → sema → codegen → emit.

### Checkpoint

The fork boundary must be a state where the frontend is between top-level
declarations: no pending token, no open scope, no in-flight template
instantiation. The initial implementation places the checkpoint
immediately after the last shared `#include`.

## Correctness

Every child's emitted `.o` must be **byte-identical** to what plain
`clang -c` produces for the same source and flags. Sources of
non-determinism (`__COUNTER__`, internal ID allocators, diagnostic
ordering) are enumerated and either neutralized or asserted stable in the
fork checkpoint.

The Step 1 milestone exists to prove this at N=2 on a controlled fixture.

## Overhead model

```
Traditional:   N × (P + U)
vane:          P + N × U + Overhead

Savings:       (N - 1) × P - Overhead
```

`Overhead` includes prefix discovery (partial preprocess of all TUs),
fork cost × N, and COW page split cost as children mutate the shared
state. When `P` is small relative to `U`, or the fork pool is
memory-bound, vane's win narrows or inverts. The `measure` command
(planned) reports empirical savings.

## Scope

vane targets **cold builds**. Incremental builds are already served well
by ccache/sccache/cHash/clang-cache. vane's fork mechanism is orthogonal
and can compose with any of those caches on the child side.

## Open problems

- Bit-exact `.o` at N=2 (Step 1).
- Bit-exact `.o` at N=64 with fork pool (Step 2).
- Cost model: does vane beat PCH on realistic workloads, and by how much?
- Cross-version maintenance: clang internal APIs change; the executor's
  clang-touching surface must stay small.

See [`platform-support.md`](platform-support.md) for platform scope.
