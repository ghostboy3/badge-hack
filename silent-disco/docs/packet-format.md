# Beacon packet format

Wire format shared by `beacon/` (sender) and `badge/` (receiver) via the
`beacon_packet/` component (`beacon_packet/include/beacon_packet.h`). Badge
and beacon must never diverge on this — that's why it lives in one shared
component instead of being copy-pasted into both projects.

## Transport

Carried as a Manufacturer Specific Data AD element inside a BLE extended
advertising packet (non-connectable, undirected), the same wrapping
`bugdex/main/ble_proto.c` uses:

```
AD element: [len][0xFF][company_id_lo][company_id_hi][payload...]
```

- `company_id` = `0xFFFF` (unassigned/prototype placeholder, same as
  bugdex). Since bugdex may be advertising on the same badges/venue, the
  magic bytes below (not the company id) are what disambiguate protocols on
  air.
- Advertised at a 100-300 ms interval (`itvl_min`/`itvl_max` 160/480 in
  0.625 ms units), redundant by nature of continuous ext-adv.

## Payload: `SD_PKT_BEACON` (0x01)

37 bytes total (v2, step 4b adds the pattern lookahead below), little-endian,
no padding:

| Bytes | Field | Type | Meaning |
|---|---|---|---|
| 0-1 | magic | `0x5D 0x15` | Fixed marker, checked before anything else |
| 2 | version | `uint8` | `2` |
| 3 | type | `uint8` | `0x01` = `SD_PKT_BEACON` (only type defined so far) |
| 4 | channel_id | `uint8` | Fixed `0` in step 1 (single channel) |
| 5 | bpm | `uint8` | Fixed `120` in step 1 |
| 6-13 | beat_timestamp_us | `int64` | Beacon's own `esp_timer_get_time()` value at beat 0. Set once at boot; the fixed anchor all beat phase is computed from. |
| 14-21 | send_time_us | `int64` | Beacon's own `esp_timer_get_time()` value when *this* packet was assembled. Refreshed on every broadcast (~150 ms), unlike `beat_timestamp_us`. |
| 22-36 | segments[3] | 3x `(uint8, uint32)` | Pattern lookahead — see below. |

`beat_timestamp_us` and `send_time_us` are both in the beacon's own local
clock (microseconds since the beacon booted) — they are **not** wall-clock
or synced to anything until the badge processes them.

## Why two timestamps

`beat_timestamp_us` alone can't be used by the badge: it's a beacon-local
clock value, and the badge has no independent way to map beacon-clock onto
its own clock from a single static number. `send_time_us` gives that
mapping: it's the beacon-clock reading at (approximately) the same physical
instant the badge receives the packet, so the badge can compute:

```
raw_offset = badge_local_recv_time - send_time_us
```

`raw_offset` estimates the beacon-to-badge clock skew plus radio/processing
latency. Latency only ever adds delay, so across several packets the
**minimum** observed `raw_offset` (within a trailing ~5 s window) is the
best available estimate — this is the "favor earliest arrivals" rule from
`CLAUDE.md`.

Once the badge has an offset estimate, it can locate any beat:

```
beacon_time_now_est = badge_local_now - offset_estimate
beat_period_us       = 60_000_000 / bpm
phase_us              = (beacon_time_now_est - beat_timestamp_us) mod beat_period_us
```

`phase_us` is 0 exactly on a beat and wraps at `beat_period_us`; the badge
lights its LEDs while `phase_us` is under a short pulse width.

## Beacon restart / reconfig detection

If a received packet's `beat_timestamp_us` or `channel_id` differs from the
badge's currently stored reference, the badge treats it as the beacon
having restarted or been reconfigured: it discards its offset history and
re-anchors from scratch rather than blending old and new references.

## Pattern lookahead (step 4b, CLAUDE.md rule 1)

"Broadcast a schedule, not events" — the beacon never sends "drop now"; it
sends the current segment plus the next `SD_SCHEDULE_LOOKAHEAD` (3)
upcoming ones, each as `(type: uint8, start_beat: uint32)`. `start_beat` is
an absolute beat index (0 = the beat at `beat_timestamp_us`). `type` is one
of `SD_SEG_STEADY` (0), `SD_SEG_BUILDUP` (1), `SD_SEG_DROP` (2),
`SD_SEG_BREAKDOWN` (3).

`segments[0]` is always the segment covering "now" as of `send_time_us`;
`segments[1]`/`[2]` are the next two after it, each strictly later than the
one before. A badge that hasn't heard a packet in a while keeps free-running
against the last list it has — since segment boundaries are absolute beat
numbers, not relative offsets, this degrades gracefully: the badge just
runs past the known segments' boundaries and holds at the last one until a
fresh packet arrives (rendering falls back to steady behavior once the beat
index runs past all 3 known boundaries — see `ble_sync_current_pattern()`).

The beacon computes this list from a small deterministic "song structure"
generator (`beacon/main/pattern.c`) standing in for a real DJ- or
track-analysis-driven schedule (CLAUDE.md rule 5's offline pre-analysis
pipeline is future work). Badges never see that generator or its cycle
length — they only ever consume whatever segment list was last broadcast —
so swapping in a real schedule later only touches the beacon.

## Future extensions

- `channel_id`/`bpm` becoming genuinely variable per channel, each with its
  own beat timeline and pattern lookahead (currently all 3 channels share
  one timeline; only their rendered color differs — see `badge/main/main.c`).
- A CATCH_*-style ack channel is not planned; phones and badges never talk
  to each other per the architecture doc.
