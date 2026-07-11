# Vane spikes: fork-after-prefix feasibility

Throwaway experiments validating the core mechanism behind Vane
Architecture V1 (`docs/architecture-v1.md` context): parse a shared
compilation prefix once in a `clang::Interpreter`, then `fork()` and
continue each translation unit independently in a child process, relying
on Linux copy-on-write to share the already-parsed Clang/AST state.

| Spike | Question | Verdict |
|---|---|---|
| `001-clang-interpreter-fork` | Does fork() after Parse()+Execute() (JIT) survive without crashing/deadlocking? | VALIDATED (toy scale) |
| `002-fork-codegen-object` | Does the same mechanism produce real, linkable `.o` files equivalent to independent compiles? | VALIDATED — byte-identical disassembly vs. independent compiles |
| `003-fork-large-prefix` | Does it still hold and still pay off with a realistic large prefix (`bits/stdc++.h`)? | VALIDATED — ~4x faster than `-j2` baseline, ~6x vs `-j1`, behavior-equivalent output |
| `004-vane-vs-pch` | With a synthetic prefix, does vane's fork-COW actually beat **PCH** (the real competitor), not just no-cache builds? | VALIDATED (conditionally) — ~43% faster than PCH at `-j32`, verified byte-equivalent symbols + identical program output. Caveats: `-O0` only, synthetic prefix-heavy fixture. |

See each spike's own `README.md` for exact build commands and real
command output.

Spikes 001-003 were run on a 2 vCPU VPS (see below). Spike 004 was run on
a 32-core / 15 GB WSL2 Debian box with clang/LLVM 19.1.7, where
`libclang-cpp.so` *does* ship an unversioned symlink, so it links with
plain `-lclang-cpp -lLLVM`.

## Environment used
- Debian 12 VPS, 2 vCPU / 4GB RAM
- `clang++-19` / `libclang-19-dev` (needed for `clang/Frontend/*.h`,
  `clang/Interpreter/*.h` — not installed by default, only
  `libclang-common-19-dev` was present)
- Link flags use versioned `.so` names because Debian's package ships no
  unversioned `libclang-cpp.so` symlink:
  `-l:libclang-cpp.so.19.1 -l:libLLVM-19.so`

## Known gaps not yet covered by any spike
- Automatic detection of which TUs in a `compile_commands.json` can safely
  share a prefix (flag/include-path compatibility grouping).
- Diagnostics/error-path behavior when a forked branch has a compile error.
- Fork-count scaling beyond 6 branches.
- Real project headers/templates as the prefix (only stdlib tested so far).
- clang::Interpreter API stability across LLVM versions (currently pinned
  to LLVM 19; this is an internal REPL-oriented API, not a stable public
  one).
