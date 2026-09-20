# Silent Disco web app

Steps 2 & 4a per `../CLAUDE.md`'s build order: a master-clock server and a
phone page that syncs to it over WebSocket, then schedules Web Audio
playback (clicks on the beat) using that synced clock, across three
tappable channels. Protocol details: `../docs/web-sync-protocol.md`.

Fixed 120 BPM for all three channels, matching `../badge/` and
`../beacon/`'s firmware -- per-channel tempo and the lookahead pattern
schedule (build-ups, drops, breakdowns) are the next build-order step.
There are no real tracks yet; each channel is a distinct synthesized tone
standing in for one, scheduled through its own gain node exactly as
CLAUDE.md describes ("preloads all 3 tracks, plays them in lockstep at gain
0, and crossfades to the selected one") so the real audio can drop in later
without changing this structure.

## Run

```sh
npm install
npm start   # listens on :3000, override with PORT=xxxx
```

Open `http://<host>:3000/` on a phone on the same network, tap **Join the
disco**, and you should hear/see 120 BPM clicks with a synced pulse. The
stats panel shows the estimated clock offset and best round-trip time.
