# Phone clock-sync protocol (web app)

Build-order step 2: the web server holds a master clock; phones sync their
local clock to it over WebSocket, then schedule Web Audio playback using
that synced clock — never `setTimeout` (CLAUDE.md's working agreement).

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
{ "type": "hello", "channelId": 0, "bpm": 120, "beatEpochMs": <number> }
```
`beatEpochMs` is the server's own `Date.now()` at the instant it booted --
the fixed anchor "beat 0", analogous to the beacon's `beat_timestamp_us`.

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

## Scheduling playback

`beatPeriodMs = 60000 / bpm`. On each scheduler tick (~25 ms, via
`requestAnimationFrame`, not `setInterval`/`setTimeout` for the audio
scheduling itself):
```
serverNow      = Date.now() + offsetEstimate
nextBeatTime   = beatEpochMs + ceil((serverNow - beatEpochMs) / beatPeriodMs) * beatPeriodMs
msUntilBeat    = nextBeatTime - serverNow
audioTime      = audioCtx.currentTime + msUntilBeat / 1000
```
Any beat whose `audioTime` falls within a short lookahead window (~200 ms)
and hasn't been scheduled yet gets `oscillator.start(audioTime)` -- the
standard Web Audio look-ahead scheduler pattern. `msUntilBeat` is a delta
between two same-units server-clock timestamps, so the client-side offset
cancels out; no need to correlate `audioCtx.currentTime` against wall-clock
time separately.

## Known gap (not solved in step 2)

`beatEpochMs` (web server) and `beat_timestamp_us` (beacon firmware,
`docs/packet-format.md`) are independent anchors — each is just "when that
process booted." Phones and badges will each be internally in time with
120 BPM, but not necessarily phase-aligned with each other until both are
started from a shared real-world reference. CLAUDE.md doesn't specify that
mechanism yet either; flagged here as an open point for a later step, not
attempted now.
