# Measurements

The current badge (`badge/`) doesn't sync to anything — it's a standalone
breathing-fade light show, so there's nothing to measure an offset against.

The tables below were for the earlier beat-synced badge design (BLE beacon
-> badge LED flash, and a per-headphone Sync check offset), which no longer
applies to the current badge build. `beacon/` still works standalone if
that design comes back later; keeping the method notes here for that case.
No data was ever recorded in either table.

## Badge LED sync to fixed 120 BPM beacon (not applicable to the current badge)

| Date | Setup (distance, obstructions, # badges) | Measured offset (ms) | Method | Notes |
|---|---|---|---|---|
| | | | | |

Method notes:
- Offset between two badges: film both in slow-mo (120/240 fps) next to
  each other, measure frame delta between their LED pulses.
- Audio-to-light offset: film a badge in slow-mo next to a phone/speaker
  playing a 120 BPM click track, measure frame delta between the audible
  click and the LED pulse.
- Also record behavior when the beacon is power-cycled mid-run (should
  re-anchor within a few seconds, no visible glitch beforehand).

## Per-badge Sync check offsets (feature removed)

Was dialed in per headphone model via the badge's Sync check screen
(Home to enter, Up/Down to adjust) -- removed along with the badge's BLE
beat sync.

| Headphone model | Nudged offset (ms) | Notes |
|---|---|---|
| | | |
