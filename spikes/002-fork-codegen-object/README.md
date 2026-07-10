# Spike 002: fork-after-prefix with real object-file codegen (not JIT)

## Question
Spike 001 proved fork-after-prefix works when the children JIT-execute code
in-process. But Vane needs real `.o` files a normal linker can consume. Does
the same fork-after-prefix mechanism still work when children instead run
LLVM's standard `TargetMachine::addPassesToEmitFile` codegen pipeline (the
same one `llc`/`clang -c` uses) on their `PartialTranslationUnit.TheModule`?
And is the resulting object code byte-for-byte equivalent to what you'd get
from fully independent `clang++ -c` compiles of the same source?

## Approach
- `main.cpp`: parent parses+codegens a shared prefix (a template class +
  helper function) to `/tmp/spike002_prefix.o`. Then forks twice; each child
  parses its own branch code (calling the shared helper) and codegens its
  own `.o`.
- `normal/`: the same three logical translation units (prefix, branch A,
  branch B) written as ordinary standalone `.cpp` files, compiled
  independently with plain `clang++-19 -std=c++17 -c`, as the baseline for
  comparison.
- `driver.cpp`: a tiny `main()` that calls into all three functions, used to
  link-and-run both the fork-produced objects and the baseline objects.

## Build & run
```bash
clang++-19 -std=c++17 main.cpp \
  -I/usr/lib/llvm-19/include -L/usr/lib/llvm-19/lib \
  -l:libclang-cpp.so.19.1 -l:libLLVM-19.so \
  -Wl,-rpath,/usr/lib/llvm-19/lib -o fork_codegen
./fork_codegen   # produces /tmp/spike002_{prefix,child_a,child_b}.o

clang++-19 -std=c++17 -c driver.cpp -o /tmp/spike002_driver.o
clang++-19 /tmp/spike002_driver.o /tmp/spike002_prefix.o \
  /tmp/spike002_child_a.o /tmp/spike002_child_b.o -o /tmp/spike002_linked
/tmp/spike002_linked   # => 14 21 42

# baseline
cd normal
clang++-19 -std=c++17 -c prefix.cpp -o /tmp/normal_prefix.o
clang++-19 -std=c++17 -c a.cpp -o /tmp/normal_a.o
clang++-19 -std=c++17 -c b.cpp -o /tmp/normal_b.o
clang++-19 /tmp/spike002_driver.o /tmp/normal_prefix.o /tmp/normal_a.o /tmp/normal_b.o -o /tmp/normal_linked
/tmp/normal_linked   # => 14 21 42
```

## Actual results (real run)
- Both linked binaries print the same three values: `14`, `21`, `42`.
- `llvm-nm -C` on each pair of objects (fork-produced vs. independently
  compiled) shows identical symbol tables.
- `llvm-objdump -d` on each pair shows **byte-for-byte identical
  disassembly** (only trivial ELF metadata/size differences, e.g. debug
  section padding — no instruction differences).

## Verdict: VALIDATED
Fork-after-prefix, when children run the standard codegen pipeline instead
of JIT, produces object files that are instruction-for-instruction identical
to fully independent compiles, and link/run correctly together. This is the
core mechanism Vane's architecture doc (V1) depends on — parse a shared
prefix once, fork, and have each branch emit real, standard-conforming
object code.

## Known gaps (not tested here)
- Still toy scale (prefix = one template + one function). See spike 003 for
  a stdlib-scale prefix.
- No error-path testing.
- Only 2 forks.
