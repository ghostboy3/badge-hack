# Measurements

Per `CLAUDE.md`'s working agreements: measure, don't assume. Record
inter-badge and audio-to-light offset results here as they're taken.
Targets: ~10-20 ms between badges.

## Step 1: badge LED sync to fixed 120 BPM beacon

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

## Step 3: per-badge Sync check offsets

The nudge from `docs/packet-format.md`'s badge firmware, dialed in per
headphone model via the Sync check screen (Home to enter, Up/Down to
adjust). Bluetooth audio latency is fairly consistent per headphone model,
so this table doubles as a reference for known models.

| Headphone model | Nudged offset (ms) | Notes |
|---|---|---|
| | | |
