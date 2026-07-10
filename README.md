# vane

An execution-model exploration for C++ cold-build acceleration.

vane parses the shared frontend prefix of many translation units *once* — as
a live `CompilerInstance` — then forks into per-TU children that continue
independently. The unit of sharing is **live compiler state**, not a
serialized artifact (PCH) or a cached output (ccache/cHash).

```
        P (shared preprocess + parse)
      / | \
     A  B  C   (per-TU continuation)
```

## Status

Pre-alpha. Step 1 milestone: reproduce the 2-TU fork PoC (bit-exact `.o`
against plain clang) inside this repo. See
[`docs/architecture.md`](docs/architecture.md) for the design and
[`docs/platform-support.md`](docs/platform-support.md) for supported
platforms.

## Build

Linux with clang 17 headers installed:

```sh
sudo apt install -y libclang-17-dev libclang-cpp17-dev llvm-17-dev cmake ninja-build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DClang_DIR=/usr/lib/llvm-17/lib/cmake/clang \
  -DLLVM_DIR=/usr/lib/llvm-17/lib/cmake/llvm
ninja -C build
./build/vane --version
```

## Platforms

Linux + clang is the only Tier 1 platform. WSL2 on Windows runs the same
code path. Native Windows and MSVC are out of scope — see
[`docs/platform-support.md`](docs/platform-support.md).
