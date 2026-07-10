# vane design

An opinionated view of how to turn the fork-checkpoint mechanism (validated
by [`spikes/`](../spikes/README.md)) into a usable tool. Not a
specification — the goal is to state design decisions clearly enough that
they can be argued with.

Reads assuming you've read [`architecture.md`](architecture.md) and skimmed
the spike READMEs.

## 1. What vane is (and isn't)

vane is a **build orchestrator** for C++ cold builds. It reads a
`compile_commands.json`, discovers translation units that share a large
frontend prefix, drives that prefix through a live `clang::Interpreter`
once, then `fork()`s to continue each TU independently and emit its `.o`.

The sharing unit is **live compiler state**, not a serialized artifact
(PCH), not a cached output (ccache/cHash), not a declared module (C++20
BMI). See [architecture.md](architecture.md) for the comparison table.

vane is not:

- A cache. It composes with ccache/sccache/clang-cache; it doesn't replace
  them.
- A compiler wrapper. `vane clang++ foo.cpp` is not the interaction model.
  vane's value is cross-TU sharing, which requires a whole-project view.
- A distributed build system. Remote execution is out of scope for v0.x.
- A linker or archiver. vane emits `.o` files. Linking is the build
  system's job (unchanged).
- Applicable to MSVC. See [platform-support.md](platform-support.md).
- For C++20 modules-first projects. Those already declare their sharing.

## 2. Product surface

### CLI shape

```
vane build -p <compile_commands.json | build-dir> [options]
vane plan  -p <compile_commands.json | build-dir> [options]
vane check -p <compile_commands.json | build-dir> [options]
```

- **`build`** — the primary command. Executes the plan and writes `.o`
  files exactly where each TU's original compile command said they'd go.
  From the build system's perspective, vane is a drop-in replacement for
  invoking clang on every TU: after vane exits, the `.o` files exist and
  linking proceeds normally.
- **`plan`** — dry run. Prints the flag-equivalence classes, the prefix
  tree, and the expected savings estimate, without running any compile.
  Cheap, for inspection.
- **`check`** — validation mode. Runs the plan, then re-runs plain
  `clang -c` on each TU and diffs the output. Any non-identical bytes
  fail the check with details. Used in CI and when debugging correctness
  regressions.

Global flags that matter:

```
--jobs N               fork pool size (default: memory-bounded, see §5)
--memory-budget MB     RSS ceiling for the fork pool (default: 75% free)
--prefix-strategy S    auto | fixed:<file> | none (default: auto)
--fallback CMD         command to run when vane can't handle a TU
                       (default: clang -c, the original command)
--validate             run every TU under clang -c too and diff
--log LEVEL            error | warn | info | debug (default: warn)
--profile PATH         emit a per-TU + prefix timing profile (JSON)
```

### Input contract

`compile_commands.json` (Clang's format) is the only supported input. Every
build system worth using in 2026 either emits it natively (CMake, Meson) or
can via aspects/plugins (Bazel, Buck, GN → ninja → compdb). If a project
doesn't have compile_commands.json, that's the first thing they fix before
touching vane.

### Output contract

For every entry in the input, vane guarantees one of:

1. **Success** — `.o` at the expected `output` path, byte-identical
   (modulo declared non-determinism, §6) to what `clang -c` on the same
   `arguments` in the same `directory` would have produced.
2. **Fallback** — same `.o`, produced by running the fallback command
   (default: the original clang invocation).
3. **Failure** — no `.o`. Exit code non-zero, diagnostic on stderr.

The build system can treat vane's exit strictly like ninja treats clang's
exit. **vane is never allowed to produce a wrong `.o`**. This is not a
"should"; it's an invariant enforced by §6.

## 3. Architecture

```
┌────────────────────────────────────────────────────────────┐
│  CLI (vane build/plan/check)                               │
└────────┬───────────────────────────────────────────────────┘
         │
┌────────▼───────────────────────────────────────────────────┐
│  Planner                                                   │
│  ─ compdb parse                                            │
│  ─ flag normalization                                      │
│  ─ flag-equivalence partitioning                           │
│  ─ per-partition prefix discovery (preprocess + LCP)       │
│  ─ prefix tree construction                                │
│  ─ savings estimate                                        │
└────────┬───────────────────────────────────────────────────┘
         │  PrefixTree { nodes, tu_groups, per_group_flags }
┌────────▼───────────────────────────────────────────────────┐
│  Executor                                                  │
│  ─ fork pool (memory-bounded)                              │
│  ─ per-partition Interpreter lifecycle                     │
│  ─ prefix parse + codegen                                  │
│  ─ fork checkpoint → per-TU continuation                   │
│  ─ error path + fallback                                   │
│  ─ .o writer + validator hook                              │
└────────┬───────────────────────────────────────────────────┘
         │  { .o files, per-TU status, timing profile }
┌────────▼───────────────────────────────────────────────────┐
│  Reporter                                                  │
│  ─ progress line (like ninja)                              │
│  ─ error/fallback summary                                  │
│  ─ profile JSON output                                     │
└────────────────────────────────────────────────────────────┘
```

Boundaries are strict. Planner does not fork. Executor does not read the
compdb. Reporter does not touch clang. This lets each subsystem be tested
in isolation and lets Planner run standalone (`vane plan`).

### The clang-touching layer

Everything in the Executor that touches `clang::Interpreter`, `Preprocessor`,
`CompilerInstance`, `PartialTranslationUnit`, or LLVM's codegen sits in a
single translation unit — `src/executor/clang_adapter.{h,cpp}` — with a
narrow interface:

```cpp
struct PrefixHandle;              // opaque, holds the Interpreter + parsed PTUs
struct BranchInputs { std::string code; CompileCommand cmd; };
struct BranchOutput  { std::string obj_path; Diagnostics diags; };

PrefixHandle open_prefix(const FlagSet& flags, std::string_view prefix_src);
BranchOutput continue_branch(PrefixHandle&, const BranchInputs&, std::string out);
void close_prefix(PrefixHandle&);
```

Everything above this interface is version-stable C++ we own. Everything
below can be re-implemented against a new clang major without touching the
Planner or Reporter. This is where the maintenance cost lives; keeping it
under ~800 LOC is a design constraint.

## 4. Planner

### Flag equivalence

Two TUs can share a prefix only if their preprocessing environments are
compatible. Compatibility ≠ identical: `-o` differs per TU, `-MF` differs,
paths differ. What must match:

| Flag category    | Must match | Notes                            |
|------------------|------------|----------------------------------|
| Include paths    | Yes        | `-I`, `-isystem`, `-iquote`      |
| Macros           | Yes        | `-D`, `-U`                       |
| Language         | Yes        | `-std=`, `-x`                    |
| Frontend switches| Yes        | `-fno-rtti`, `-fexceptions`, etc.|
| Target           | Yes        | `-target`, `-march`              |
| Optimization     | No*        | `-O` doesn't affect preprocess   |
| Warnings         | No         | `-W*`                            |
| Output           | No         | `-o`, `-MF`, `-MT`               |

\* Optimization doesn't affect the prefix but must be replayed per TU in
codegen. The Executor tracks it separately.

Flag normalization: canonicalize path arguments (absolute), sort `-D`
options, remove redundant `-I`. Then two TUs are in the same
**equivalence class** iff their normalized flag sets match.

### Prefix discovery

Within a class:

1. Run `clang -E` (or drive the Interpreter's Preprocessor equivalent) on
   each TU. Collect the token stream.
2. Find the longest common prefix over token streams. LCP over N sequences
   is `O(N × min_length)` naïve, or `O(total_length)` via generalized
   suffix tree; naïve is fine for N in the low thousands.
3. Locate the source position where the LCP ends. This is the
   **prefix boundary**.

Preprocessing all TUs sounds expensive. Two things save it:

- Preprocess is 5-10× cheaper than full compile (no sema, no codegen).
- The output is small enough to cache under `.vane/plan-cache/<hash>` keyed
  on `(file mtime, flag set hash)`. Second run of `vane plan` on an
  unchanged tree is near-instant.

### Prefix tree

Sharing is not necessarily flat. Real projects have:

- A project-wide common header (e.g. `common.h`) — global prefix.
- Per-module headers (e.g. `net/net_common.h`) — subgroup prefix.
- Sometimes deeper: per-directory, per-feature.

Model this as a **tree**:

```
              global-prefix
             /             \
      subgroup-A        subgroup-B
      /       \         /   |   \
   TU1     TU2       TU3  TU4  TU5
```

At each internal node, the Executor forks. A TU at depth D goes through
D forks in total. In practice we cap D at 3 — deeper trees have
diminishing returns because per-level fork overhead compounds and
per-node prefix state must live in memory across the fork wave.

Tree construction: greedy. Start with the global LCP over the whole
class. For TUs where the LCP is a strict prefix of an even-longer shared
segment with a subset of TUs, recurse on that subset. Stop when subgroup
size drops below a threshold (default 3, tunable).

### Savings estimate

For a leaf node with N TUs sharing prefix P:

```
Without vane:  N × T(P) + Σ T(U_i)
With vane:     T(P) + Σ T(U_i) + Overhead(N)
Savings:       (N-1) × T(P) - Overhead(N)
```

`T(P)` and `T(U_i)` come from the Planner's preprocess+parse timing (a
Planner byproduct). `Overhead(N)` is empirically calibrated (§8). vane
should refuse to fork a group when the estimated savings are negative or
below a threshold — plain clang is the default when vane can't help.

## 5. Executor

### Fork pool

CPU count is the wrong metric. Fork pool size is bounded by memory:

```
pool_size = min(
  cpu_count,
  (memory_budget - prefix_rss) / expected_child_delta_rss
)
```

Where:

- `prefix_rss` — measured after parsing the prefix and before the first
  fork.
- `expected_child_delta_rss` — a modest constant per child (Interpreter
  bookkeeping + branch parse + codegen module). Spike 003 showed
  ~30-50MB delta per child at stdlib scale. Start at 100MB, tune from
  telemetry.

The pool is not a fixed-size pool of workers. It's a **wave scheduler**:
fork k children, wait for them to finish (or reach a checkpoint if we
add nested trees), fork the next k. A worker that finishes early is
picked up as soon as memory permits.

### Prefix lifecycle

For each flag-equivalence class:

1. Build a fresh `clang::Interpreter` with the class's flags.
2. `interp.Parse(prefix_src)` — this is the expensive step.
3. Emit `prefix.o` (only if the prefix contains code that must survive
   linking, which for `bits/stdc++.h` + a shared helper it usually
   does).
4. Fork loop.
5. In each child: `interp.Parse(branch_src)` + emit `branch.o` +
   `_exit`.
6. Parent `waitpid` all, aggregate results.

The `Interpreter` is not reused across classes. Each class starts fresh.

### Prefix source

The prefix source itself is not a magic input — it's just the tokens up
to the boundary that the Planner discovered. In the concrete
implementation:

```cpp
// Reconstruct prefix source from the preprocess dump up to the LCP end.
std::string prefix = preprocess_prefix(class_representative_TU, lcp_end);
interp.Parse(prefix);
```

For each branch, the source is the remaining tokens of that TU:

```cpp
std::string branch = preprocess_suffix(tu, lcp_end);
```

This reduces both sides to plain source strings, which the Interpreter
can `Parse()` cleanly. It also decouples us from any assumption about
Preprocessor state after the fork boundary — we replay from raw source.

### Error path

A child fails to compile:

- Diagnostics are captured in the child, serialized to a pipe, and
  emitted by the parent as if `clang -c` produced them.
- The child exits with a non-zero status.
- The TU is marked failed. **No fallback for compile errors** —
  compile errors are the user's problem, not vane's.

A child crashes (SIGSEGV, etc.):

- The parent detects `WIFSIGNALED`.
- Fallback: run the original `clang -c` command for that TU. If that
  also fails, emit both and let the user file a bug.
- Log the crash with the prefix hash, branch source hash, and clang
  version — enough to reproduce.

A prefix fails to parse:

- The entire class falls back to plain `clang -c` per TU.
- Log and continue.

The invariant: **vane exits with success only if every TU has a
correct `.o`, however produced.**

## 6. Correctness

Bit-exact `.o` is a hard invariant, not aspirational. It's how the build
system trusts vane.

### Declared non-determinism sources

These are known to introduce byte-level differences between vane's output
and plain `clang -c` output, and are neutralized before comparison:

- `__DATE__`, `__TIME__`, `__TIMESTAMP__` — normalized in both sides via
  `-Wno-builtin-macro-redefined -D__DATE__="\"redacted\"" -D__TIME__=...`
  in `--validate` mode.
- Debug info paths — normalized via `-fdebug-prefix-map`.
- ELF section padding, timestamps in `.note.gnu.build-id` — stripped
  before comparison (compare `.text` and symbol tables, not the whole
  file).

Sources that must be zero and are asserted stable:

- Instruction bytes in every function.
- Symbol table (names, types, sections).
- Relocation entries.
- `.rodata` contents.

`vane check` runs a full comparison and reports any deviation with a
disassembly diff.

### __COUNTER__ handling

`__COUNTER__` is per-TU-scoped in clang. Post-fork, each child's
Interpreter has its own copy of the counter state (COW split at first
mutation). Because we replay source from strings rather than continuing
mid-token-stream, and each child parses its own suffix from scratch, the
counter starts at the value it was left at when the prefix ended —
which is the same value plain clang would see at that point in an
equivalent source. **Verified in spike 002 via byte-identical
disassembly**; keep the check permanent in `vane check`.

### Random-name symbols

Certain constructs (lambdas at namespace scope, anonymous unions in
templates) produce names derived from source location. Because our
source replay preserves source locations byte-for-byte, these come out
identical. Regression risk: any future optimization that "reformats" the
suffix before feeding it to the Interpreter would break this. **Rule:
suffix bytes are passed through untouched.**

## 7. Composition with the ecosystem

### ccache / sccache

vane is not a cache; it composes with one:

```
build system
  → vane (fork-checkpoint mechanism)
    → per-TU:  ccache clang -c ...  (in fallback path)
    → per-fork-child: (bypass, since we're already emitting via LLVM directly)
```

In the primary path (child emits `.o` from the Interpreter's PTU), ccache
never sees the invocation. This is correct: vane's produced `.o` is by
definition equivalent to `clang -c`, and if the same code+flags are
compiled again next run, vane will produce the same `.o` again — no
cache miss cost, no cache hit needed.

In the fallback path, ccache is a plain compiler-launcher wrapper
around the fallback command. No special integration.

### CMake / GN / Bazel

Use `CMAKE_CXX_COMPILER_LAUNCHER` and equivalents:

```cmake
set(CMAKE_CXX_COMPILER_LAUNCHER vane invoke)
```

`vane invoke` is a mode where vane accepts a single compile invocation,
delegates it to plain clang (no orchestration possible from a single
invocation), and records it. **This is only useful for measurement/audit
purposes — the primary use of vane is not per-invocation.**

The primary integration is:

```cmake
add_custom_target(vane_build
  COMMAND vane build -p ${CMAKE_BINARY_DIR}
  BYPRODUCTS ...
  DEPENDS ...
)
```

Or, more cleanly, invoke `vane build` before `ninja` on the linking
targets. The user just adds a script step to their CI/dev workflow.

### C++20 modules

Modules-first projects don't benefit from vane — their sharing is
explicit and BMI-based. vane detects `-fmodules` and skips fork-planning
for module-consuming TUs, falling back to plain clang. Mixed projects
(some module-using TUs, some header-using) get vane treatment on the
header-using subset only.

## 8. Observability

`--profile` emits a JSON file per run:

```json
{
  "vane_version": "0.1.0",
  "started_at": "...",
  "duration_ms": 12450,
  "classes": [
    {
      "flags_hash": "...",
      "tu_count": 234,
      "prefix_boundary": ".../src/common.h:812",
      "prefix_parse_ms": 1873,
      "prefix_rss_kb": 412000,
      "children": [
        { "tu": "src/net/foo.cpp", "parse_ms": 22, "codegen_ms": 41, "delta_rss_kb": 38000, "outcome": "ok" },
        ...
      ]
    }
  ],
  "totals": { "wall_ms": 12450, "cpu_ms": 84210, "savings_vs_estimated_baseline_ms": 61200 }
}
```

This is the debugging surface. Anyone reporting "vane didn't help my
project" ships this file first.

## 9. Non-goals for v0.x

Explicitly out of scope for the first year:

- **Distributed / remote execution.** The mechanism is fork-based
  in-process; remote execution requires a different sharing model. Not
  worth conflating.
- **Persistent state across invocations.** No `.vane/interp-cache/`
  keeping parsed prefixes across runs. Every invocation rebuilds. Might
  add later.
- **Automatic error recovery beyond fallback.** No "try to salvage a
  crashed child by retrying with adjusted flags."
- **Incremental / dependency tracking.** vane compiles what the build
  system tells it to compile. It doesn't decide what needs recompiling.
- **Header-Modules interop.** Module-using TUs are excluded from
  fork-planning. Real Modules integration is v1+.
- **Support for `clang::Interpreter` API drift.** We pin LLVM 19. When
  LLVM 20+ is worth targeting, we bump the pin and re-verify. No
  version compatibility layer.

## 10. Path from spike to v0.1

Not a project plan, a sequence of milestones. Each milestone is defined
by what's verifiable at the end.

**M1 — CLI skeleton + Planner (no execution)**
Deliverable: `vane plan -p <compdb>` prints flag classes, prefix tree,
savings estimate. `vane build` is stubbed. Working on the LLVM
compile_commands.json.

**M2 — Executor for one flag class, flat prefix**
Deliverable: `vane build` handles a single flag-equivalence class with
one shared prefix and N branch TUs. Emits correct `.o`. Falls back on
error. Passes `vane check` on a small fixture.

**M3 — Fork pool + memory budget**
Deliverable: vane scales past the memory limit gracefully. Tested at
N=64 on a real project.

**M4 — Multi-class support**
Deliverable: real projects with heterogeneous flags across TUs are
handled. Each class runs its own Interpreter.

**M5 — Nested prefix tree**
Deliverable: depth-2 prefix tree working. Depth-3 optional. Verified
`vane check` still passes.

**M6 — LLVM self-build benchmark**
Deliverable: vane builds LLVM (or a large subproject of it) from a
clean tree. Wall-time comparison vs `ninja -jN`. This is the number
the README's headline data comes from.

**v0.1 release** — after M6, with README, docs, packaging, and a
public benchmark writeup.

Everything past v0.1 (nested trees deeper than 2, distributed, modules,
persistent state) is a v0.2+ conversation informed by what M6 shows.

---

## Open design questions I haven't resolved

Filed here so they don't quietly become defaults:

1. **Where does the prefix boundary live semantically?** Right now the
   Planner returns a source position. But is a mid-header position ever
   legal (i.e. can we fork inside `#include <vector>`)? I lean no —
   only fork at include-directive boundaries, never inside a header.
   Verify with spike-like experiment before committing.

2. **How to handle `#pragma once` interactions with source replay?**
   Should work naturally because we replay whole source, but haven't
   verified.

3. **`clang::Interpreter` and `-fPIC`, `-fPIE`, `-fvisibility=hidden`.**
   Which of these affect the parse vs the codegen? The Executor needs
   to know which to set on the shared Interpreter vs which are per-TU.

4. **Diagnostics ordering across fork children.** Two children can emit
   diagnostics concurrently; do we serialize by TU order to match plain
   clang output, or interleave with clear labels? Plain clang output
   determinism argues for the former.

5. **Windows-native path via in-memory PCH.** Explicitly deferred, but
   the Executor interface should be shaped so the Windows executor can
   slot in without rewriting Planner. Design the `PrefixHandle`
   abstraction with that in mind, don't lock it to fork().
