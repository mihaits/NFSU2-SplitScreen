# Open issues and leads

- **Light streaks without overbright** in split views ([rendering](rendering.md)).
- **Results ambient sound stutter (3P).** Continuous ~2 Hz gating, music off. Not frame hitches, sound fs handles, the
  nearest-player functions or ListenerUpdate (each was reverted live with no change). Left to check: the relocated
  listener arrays, or the original 2P behaviour.
- **Black results background**, seen once after a 2P race. If it happens again, capture state while it is still
  happening (g_currentView, views 1-4 active/rt, viewport).
