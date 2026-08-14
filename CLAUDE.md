# Gravitino DJ — agent guide

Open-source DJ software (Qt 6, C++20, macOS-first). Headline feature:
record a mixing transition once, replay/teach it beat-perfectly.

**Read before coding:** docs/ARCHITECTURE.md (module contracts, threading),
docs/TRANSITION_FORMAT.md (.gvt spec), docs/STATUS.md (who owns what, current
state — keep it updated).

Rules:
- Headers marked `// PINNED INTERFACE` are contracts; don't change signatures
  without updating all consumers and the docs in the same commit.
- All control actions go through ControlBus — never bypass it.
- Audio callback: no locks, no allocation.
- Build: `cmake -B build -G Ninja && cmake --build build`; test with `ctest
  --test-dir build` and `./build/gravitino --selftest`.
- Only edit files your task owns (see STATUS.md table) to avoid conflicts.
