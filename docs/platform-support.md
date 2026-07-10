# Platform support

vane's core mechanism — forking a live `CompilerInstance` at a shared
prefix checkpoint — depends on POSIX `fork()` with copy-on-write memory.
That constraint fixes the platform matrix.

## Tiers

| Tier | Platform                    | Status         |
|------|-----------------------------|----------------|
| 1    | Linux + clang               | Supported      |
| 2    | macOS + clang               | Best-effort    |
| 2    | Windows via WSL2 + clang    | Same as Tier 1 |
| —    | Native Windows + clang-cl   | Out of scope   |
| —    | Native Windows + MSVC       | Out of scope   |

Tier 1 is the only configuration CI gates on. Tier 2 receives fixes when
they are cheap; regressions do not block a release.

## Why not native Windows

Windows has no `fork()`. Alternatives were considered and rejected:

- **Cygwin fork emulation** — copies process memory manually. Too slow
  and fragile for compiler-scale state (hundreds of MB).
- **`RtlCloneUserProcess`** — undocumented NT API. Unsuitable for a
  production tool.
- **Multi-threaded deep-clone of `CompilerInstance`** — clang is not
  thread-safe, and cloning the entire frontend state graph is a
  multi-year rewrite.
- **In-memory PCH via section objects** — feasible, but degrades vane's
  architecture to "PCH with dynamic boundary discovery" and loses the
  no-serialization advantage. Deferred to a possible future v2.

WSL2 satisfies the fork requirement while running on Windows hardware,
and is the recommended path for Windows developers.

## Why not MSVC

MSVC is closed source, has no `CompilerInstance`-equivalent, and offers
no plugin API into its frontend. vane cannot integrate at any level. This
is not fixable from outside Microsoft.

## macOS caveats

`fork()` works on macOS but Grand Central Dispatch and Foundation runtime
components are unsafe after fork. clang itself does not use these in the
compile path, so the fork checkpoint mechanism is expected to work.
Support is best-effort until we exercise it on real workloads.
