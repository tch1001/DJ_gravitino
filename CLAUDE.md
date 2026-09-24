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

## User-data safety: beat grids are not disposable analysis

- Never edit the user's transition recipes, permanent hot cues, saved loops or
  effective song grids without explicit permission for those exact changes.
  Permission for one repair does not carry over to later tasks. Test with copies.
- A song's beat-zero anchor defines the coordinates of every transition using
  it. Moving it to the current playhead is not merely a phase adjustment and can
  break otherwise-correct recipes. Preserve effective BPM/anchors across tag,
  timestamp, cache-schema and analyzer changes; keep detector results separate.
- Do not invalidate saved setup just because the audio file mtime changed. Use
  the cache identity checks and fail closed on unverifiable replacement audio.
  Archive existing setup before overwriting it; never discard caches as a fix.
- For an authorized grid repair, close Gravitino safely first, back up the exact
  caches/catalog, update only the approved grid fields in both, and verify recipe
  hashes plus unchanged cue/loop fields. Legacy-converted recipes may have zero
  reference anchors because evidence was absent: do not treat those as reliable
  restoration targets. Provide an incoming/outgoing listening checklist.
- The September 2026 Low/S&M/How We Party incident was repaired by preserving
  user setup on refresh, then separately restoring approved anchors. The user
  confirmed the repaired transitions sound correct. Keep the cache identity and
  beatgrid-persistence regression tests passing before changing this path.
