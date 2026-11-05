# Contributing to NebulaBT

Thanks for your interest! NebulaBT is a young project and every kind of
contribution helps: bug reports, documentation, examples, and code.

## Getting started

```bash
git clone https://github.com/Super-DDG/NebulaBT.git
cd NebulaBT
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```

Requirements: C++17 compiler, CMake ≥ 3.16. BehaviorTree.CPP 4.9 is fetched
automatically (or set `NEBULABT_BT_CPP_SOURCE_DIR` to a local copy).

## Ground rules

1. **Every PR must pass `ctest` and `nebula validate`.**
   Tree files and packs are validated at load time on purpose — if your
   example ships a broken tree, the build should say so, not the robot.
2. **Design changes need an ADR.**
   Significant decisions (new subsystems, API shape, semantics) are recorded
   in `docs/adr/NNNN-title.md` before or with the implementation. Look at
   ADR-001..003 for the format: context → decision → consequences.
3. **C++17 only.** No coroutines, no C++20 features. The callback-continuation
   node API (ADR-002) is the async story — don't reintroduce polling.
4. **Don't modify BehaviorTree.CPP.** It is a package dependency (ADR-001).
   Framework-level needs go into the NebulaBT layer.
5. **SemVer discipline.** Anything promised in a public header is API.
   Prefer under-promising; never break silently.
6. **Tests travel with features.** New nodes need at least: happy path,
   failure path, cancel/halt behavior. Look at `tests/test_core.cpp` for the
   pattern (plain asserts, no test framework dependency).

## Areas that especially need help

- More industry examples (game AI, backend services) for `examples/`
- A self-hosted web visualizer for `explain()` reports (Groot2 is closed
  source — see DESIGN.md §strategic weakness)
- Python bindings behind the C ABI boundary
- Windows/macOS CI

## Reporting bugs

Open an issue with: what you submitted, what you expected, what happened,
and (crucially) the output of `nebula explain <journal>` if you have a
journal — structured failures are exactly what makes NebulaBT debuggable.

## License

By contributing you agree that your contributions are licensed under
Apache-2.0, the project license.
