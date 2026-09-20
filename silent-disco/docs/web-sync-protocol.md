# Phone clock-sync protocol (web app)

The web server holds a master clock; phones sync their local clock to it
over WebSocket, then use that synced clock to figure out where they should
be in their selected channel's playlist — real tracks (`web/public/audio/*`),
not a beat-grid click. Playback isn't quantized to any BPM grid; the only
thing kept in sync across phones is "which track, and how far into it."

This is the phone-side analogue of `docs/packet-format.md`'s badge-side
offset estimation: same idea (several round trips, favor the lowest
latency, free-run between samples), different transport (WebSocket
request/response instead of a one-way BLE broadcast, so we get a real round
trip and can use a standard NTP-style offset formula instead of a
one-way-only minimum).

## Messages

All messages are JSON text frames.

Server -> client, once, right after the socket opens:
```
{
  "type": "hello",
  "playlistEpochMs": <number>,
  "channels": [
    { "playlist": [ { "url": "/audio/pink/01-song.mp3", "title": "Song", "durationSec": 213.4 }, ... ] },
    { "playlist": [...] },  // orange
    { "playlist": [...] }   // purple
  ]
}
```
`playlistEpochMs` is the server's own `Date.now()` at the instant it
booted — the fixed anchor each channel's playlist loops from. `channels` is
built by scanning `web/public/audio/{pink,orange,purple}/` at startup (see
`web/public/audio/pink/README.md`) — an empty folder just means an empty
`playlist` array for that channel, not an error.

Client -> server, repeated every ~1s while syncing/running:
```
{ "type": "ping", "t0": <client Date.now() at send> }
```

Server -> client, reply to each ping:
```
{ "type": "pong", "t0": <echoed>, "t1": <server Date.now() at receipt> }
```

## Offset estimation (client side)

On receiving a `pong` at client time `t2`:
```
rtt    = t2 - t0
offset = ((t1 - t0) + (t1 - t2)) / 2   // standard NTP-style offset, assumes symmetric delay
```
Keep a small window of recent `(offset, rtt)` samples; the current estimate
is the offset from the sample with the **lowest `rtt`** in that window (the
round-trip analogue of "favor earliest arrivals" -- lowest RTT means least
latency-induced error). Between samples, free-run: `serverTimeNow =
Date.now() + offsetEstimate`.

## Playlist position (client side)

Each channel's playlist loops forever starting at `playlistEpochMs`. Given
`serverNow = Date.now() + offsetEstimate`:
```
total   = sum(playlist[i].durationSec)          // 0 if the channel has no tracks yet
elapsed = ((serverNow - playlistEpochMs) / 1000) mod total
walk the playlist cumulatively to find which track `elapsed` falls into,
and the offset within it (elapsed minus every earlier track's duration)
```
Any two phones evaluating this at (approximately) the same `serverNow` land
on the same track at the same position — that's the entire cross-phone sync
guarantee this protocol provides now. There is no beat-grid quantization
here at all (that only ever mattered for the BLE/LED side — see "Known
gap" below); a track's own beat is whatever's actually in the recording.

**Track-boundary resync**: when a channel's `<audio>` element fires
`ended`, recompute its position fresh from `serverNow` rather than
naively starting the next track from 0. This self-corrects any drift from
slightly-wrong `durationSec` metadata or buffering hiccups without needing
continuous mid-track re-seeking.

**Channel switching**: all three channels' `<audio>` elements play
continuously from the moment you join (each independently tracking its own
playlist position), routed through a persistent per-channel `GainNode`.
Switching channels is *only* a gain crossfade between those already-correct
positions — no source changes, no resync (CLAUDE.md rule 6).

## Known gap (still true)

`playlistEpochMs` (web server) and `beat_timestamp_us` (beacon firmware,
`docs/packet-format.md`) are independent anchors — each is just "when that
process booted." Phones and badges are each internally consistent, but not
phase-aligned with each other until both are started from a shared
real-world reference. Unaffected by moving from beat clicks to real
playlists; still an open point for a later step, not attempted here.
