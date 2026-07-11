# Spike 004: vane fork vs PCH, head-to-head

## Question

Spikes 001-003 proved the fork-checkpoint mechanism works and beats
*no-cache* independent compilation. But the real competitor for cold-build
prefix-sharing is **PCH**, not naive compilation. And auto-discovering a
shared prefix from source fails on real projects (Google-style include
order puts each TU's own header first — see the LevelDB investigation
below), so vane must use a *synthetic* prefix, which is exactly what PCH
does too.

So the decisive question: **with a synthetic prefix, does vane's
fork-COW mechanism actually beat PCH?** If not, vane has no reason to
exist as a separate tool.

## Setup

- Fixture: the `pch-planner` 42-TU fixture (`test/fixtures/proj`), whose
  `common.h` is deliberately heavy (real STL + deep template recursion,
  `Fib<40>`, `Nest<T,8>`), directly included by every TU. This is a
  *known-PCH-friendly* workload — the strongest case for PCH, i.e. the
  fairest opponent for vane.
- 42 TUs in 3 groups (a/b/c), each also including its group's
  `heavy_{a,b,c}.h`.
- Synthetic prefix = union of `common.h` + `heavy_a/b/c.h`, identical for
  the PCH baseline and vane, so the *only* variable is the sharing
  mechanism.
- All configs: `-std=c++20 -O0`, emit `.o` for all 42 TUs.
- Machine: 32 cores, 15 GB RAM (WSL2, Debian, clang/LLVM 19.1.7).
- Median of 5 runs.

Three configs:

| Config | Prefix cost | Per-TU |
|--------|-------------|--------|
| A. baseline | none | `clang++ -c`, `-j32` |
| B. PCH | `clang++ -x c++-header` once per run | `clang++ -include-pch -c`, `-j32` |
| C. vane | `Interpreter.Parse(prefix)` once | `fork()` → child parses TU body → emit `.o`, pool=32 |

## Results

```
=== SUMMARY (median ms, -j32 / pool=32) ===
  baseline: 2468
  PCH:      2056   (-16.7% vs baseline)
  vane:     1161   (-53.0% vs baseline, -43.5% vs PCH)
```

Five-run spread was tight (baseline 2411-2502, PCH 2029-2158, vane
1141-1185), so the ordering is not noise.

**vane is ~43% faster than PCH on this workload.**

## Why vane wins in the parallel case

Two PCH bottlenecks that vane doesn't have:

1. **PCH build is a serial prelude.** Building the `.pch` (~900ms) runs
   while most of the 32 cores sit idle. vane's prefix parse (~800ms) is
   also serial, but it is never serialized to disk.
2. **Every TU deserializes the PCH.** 42 TUs each mmap + deserialize the
   PCH file. vane's children inherit the already-parsed in-memory state
   through fork copy-on-write — zero deserialization per TU.

In a *serial* build these effects are smaller (PCH still cuts ~48%, as
pch-planner's own benchmark shows). The gap opens up under parallelism,
which is the realistic case.

## Correctness

Not just "it ran". Verified three ways (`spike004_verify.sh`):

- **Defined-symbol sets identical** between vane's `.o` and independent
  `clang++ -c` output, for one representative TU per group (402 defined
  symbols each, byte-identical after sorting). `fn_X()` is a strong `T`
  symbol; all prefix-derived template instantiations are `W` (weak),
  exactly as in independent compilation.
- **Links successfully** — vane's 42 objects + `main.o` link with no
  undefined/duplicate symbols.
- **Program output identical** — the linked vane binary and the linked
  independent binary produce the same result.

## What this does NOT prove (caveats — do not over-extrapolate)

1. **Synthetic, prefix-heavy fixture.** The shared headers dominate each
   TU's cost; TU-unique code is tiny. Real projects have a lower
   prefix-to-total ratio, which narrows vane's edge.
2. **`-O0` only.** No optimized build tested. Under `-O2`, codegen (which
   is per-TU and not shared) grows as a fraction of total time, shrinking
   the relative value of prefix sharing. **This is the most important
   untested dimension.**
3. **N=42 on 32 cores.** N barely exceeds core count. The scaling trend
   (larger N/core ratio) is untested and would change both PCH and vane.
4. **Single prefix / single flag class.** All TUs share one prefix. Real
   projects have multiple flag-equivalence classes and multiple prefixes,
   introducing vane grouping overhead not measured here.
5. **No `-g`.** Debug info materially changes codegen and `.o` size.

## Verdict

**VALIDATED, conditionally.** With a synthetic prefix on a PCH-friendly
workload, vane beats PCH by ~43% in the parallel case, with verified
byte-equivalent output. The core premise of the project — that
fork-COW sharing is materially better than PCH's serialize/deserialize —
holds on this workload.

The caveats (esp. `-O2` and prefix-to-total ratio) must be closed before
claiming a general result. Next: re-run at `-O2` and `-g`, and on a
larger real project, before the headline number goes in the top-level
README.

## Aside: why not auto-discover the prefix (the LevelDB finding)

The original plan was to auto-discover the shared prefix from source.
That fails on real projects. LevelDB (Google C++ style) puts each TU's
*own* header first:

```cpp
// db_impl.cc
#include "db/db_impl.h"   // <- unique to this TU, line 1
#include <algorithm>
...
```

So a source-level longest-common-prefix across TUs is empty — no two TUs
start with the same include. Token-level LCP is worse: it is *unsound*
(`#define A 1` between two includes can produce identical post-processed
tokens from different preprocessor states, causing silent miscompiles).

Conclusion: prefix must be **synthetic** (a `vane_prefix.h` the tool
generates or the user supplies), exactly like PCH. That reframes vane as
"a better mechanism for applying a chosen prefix", not "automatic prefix
discovery". Which is why beating PCH (this spike) is the thing that
matters.
