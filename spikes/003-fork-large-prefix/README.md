# Spike 003: fork-after-prefix with a large real-world-ish prefix

## Question
Spike 002 proved real `.o` codegen works, but the prefix was tiny. The key
performance question for Vane is whether the mechanism still pays off when
the shared prefix is expensive to parse, like a real C++ project's common
header set. Does parse-once/fork-many still work when the prefix is
`#include <bits/stdc++.h>` (a huge libstdc++ umbrella header), and how does
it compare to normal independent compilation?

## Approach
- Parent parses `#include <bits/stdc++.h>` plus one shared helper function,
  then codegens the prefix object.
- Parent forks 6 children. Each child parses a tiny branch that uses several
  stdlib templates (`std::vector`, `std::accumulate`, `std::string`) and calls
  the shared helper, then emits its own object.
- A separate `driver.cpp` links the prefix object plus all branch objects and
  prints each branch result.
- `baseline/` contains equivalent normal header/TU sources, compiled via
  ordinary `clang++-19 -std=c++17 -c` for comparison.

## Important source note
The baseline header defines `shared_helper` as:

```cpp
extern "C" inline int shared_helper(int x) { return x * 2 + 1; }
```

Without `inline`, including that function definition into multiple baseline
TUs creates multiple strong symbol definitions and the normal linker fails.
That is expected C++ behavior and matches how header-only helpers are usually
written. In the fork pipeline, the helper naturally lives once in the prefix
object and branches reference it as an undefined external symbol.

## Main files
- `main.cpp`: fork-after-prefix implementation and codegen timing.
- `driver.cpp`: links/runs the produced objects.
- `baseline/common.h` + `baseline/tu_*.cpp`: ordinary independent compile
  baseline.

## Actual fork-pipeline timings (real run)
Representative output:

```
prefix Parse ms=1959 CodeGen ms=87
child-0 Parse ms=19 CodeGen ms=27
child-1 Parse ms=22 CodeGen ms=19
child-2 Parse ms=26 CodeGen ms=21
child-3 Parse ms=28 CodeGen ms=15
child-4 Parse ms=18 CodeGen ms=19
child-5 Parse ms=19 CodeGen ms=28
all children done
```

Total wall time for the fork pipeline was about **2.2s**, dominated by the
one-time prefix parse.

## Baseline independent compilation timings (real run)
- Serial `-j1` equivalent: **13.33s**
- Parallel `-j2` equivalent (matches this 2-core VPS better): **8.81s**

The same baseline files were compiled with:

```bash
ls tu_*.cpp | xargs -P2 -I{} sh -c 'f={}; i=${f#tu_}; i=${i%.cpp}; clang++-19 -std=c++17 -c $f -o /tmp/baseline_${i}.o'
```

## Behavior check (real run)
Both pipelines link and print exactly the same output:

```
1 3 5 7 9 11
```

## Verdict: VALIDATED at stdlib-prefix scale
For 6 simple branch TUs sharing a very large prefix, parse-once/fork-many is
about **4x faster than `-j2` normal independent compilation** on this VPS,
and about **6x faster than serial independent compilation**, while producing
objects that link and behave equivalently.

The advantage should grow with more branch TUs because the expensive prefix
parse (~1.96s here) is paid once, while each additional branch parse is only
~20-30ms in the forked child. Normal independent compilation pays the large
stdlib parse cost in every TU.

## Known gaps (not tested here)
- Only 6 branches; still need fork-count scaling tests (e.g. 50-100 TUs).
- Branch bodies are small; not yet tested on real project TUs.
- No diagnostics/error-path testing.
- No compile-commands grouping or automatic prefix-boundary detection yet.
