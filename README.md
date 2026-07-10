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

Pre-alpha. The fork-checkpoint mechanism is validated end-to-end by three
spikes in [`spikes/`](spikes/README.md):

- **001** — fork() after `clang::Interpreter::Parse()+Execute()` works
- **002** — real `.o` codegen after fork is byte-identical to `clang -c`
- **003** — 6-way fork with `bits/stdc++.h` prefix runs ~4x faster than
  `-j2` independent compilation on a 2-vCPU VPS

The vane CLI that turns this into a usable tool (compile_commands.json
input, automatic prefix discovery, error handling, scaling beyond 6
branches) is the current work. See
[`docs/architecture.md`](docs/architecture.md) and
[`docs/platform-support.md`](docs/platform-support.md).

## Build

Linux with LLVM 19 headers installed:

```sh
sudo apt install -y libclang-19-dev libclang-cpp19-dev llvm-19-dev clang-19 cmake ninja-build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DClang_DIR=/usr/lib/llvm-19/lib/cmake/clang \
  -DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm
ninja -C build
./build/vane --version
./build/spikes/vane_spike_003 6   # replay the large-prefix benchmark
```

LLVM 19 is required because the spikes (and the eventual executor) use
`clang::Interpreter`, an internal API that shifts across LLVM versions.

## Platforms

Linux + clang is the only Tier 1 platform. WSL2 on Windows runs the same
code path. Native Windows and MSVC are out of scope — see
[`docs/platform-support.md`](docs/platform-support.md).
