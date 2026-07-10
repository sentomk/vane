# Spike 001: fork-after-prefix feasibility (clang::Interpreter + JIT)

## Question
Given a single `clang::Interpreter` that has parsed and JIT-executed a shared
"prefix" translation, when the process `fork()`s, do the resulting child
processes each get a working, independent copy of that Clang/JIT state via
Linux COW — without crashing or deadlocking?

## Approach
- Build a `clang::IncrementalCompilerBuilder` + `clang::Interpreter` (the same
  API that backs `clang-repl`).
- `Parse()` + `Execute()` (JIT) a shared helper function in the parent.
- `fork()` twice. Each child `Parse()`s + `Execute()`s (JIT) its own
  distinct code that calls the shared helper, with a different constant, to
  make sure the two children don't silently share/corrupt state.

## Build
```bash
clang++-19 -std=c++17 main.cpp \
  -I/usr/lib/llvm-19/include \
  -L/usr/lib/llvm-19/lib \
  -l:libclang-cpp.so.19.1 \
  -l:libLLVM-19.so \
  -Wl,-rpath,/usr/lib/llvm-19/lib \
  -o fork_interp
```
Note: Debian's `libclang-19-dev` package ships versioned `.so` files with no
unversioned symlink, hence `-l:libclang-cpp.so.19.1` instead of `-lclang-cpp`.

## Actual output (real run)
```
parent prefix parsed and executed
child-A value=43
child-B value=44
child-A status=0 child-B status=0
```

## Verdict: VALIDATED (at toy scale)
- fork() after Parse()+Execute() (JIT) does NOT crash or deadlock, even
  though LLJIT/ORC likely has internal background threads.
- The two children get independent, correct results (43 = 42+1, 44 = 42+2),
  proving COW state sharing works and the children don't corrupt each
  other's state.

## Known gaps (not tested here)
- Scale: prefix here is a single trivial function. Real project prefixes
  (stdlib, project headers) are orders of magnitude larger — see spike 003.
- This spike uses JIT execution (LLJIT/ORC), not real object-file codegen.
  Real build output needs `.o` files linkable by a normal linker — see
  spike 002.
- No error-path testing (what happens if a child's code fails to compile).
- Only 2 forks; no test of fork count scaling.
