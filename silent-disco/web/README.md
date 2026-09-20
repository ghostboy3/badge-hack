# Silent Disco web app

A master-clock server and a phone page that syncs to it over WebSocket,
then plays a real playlist per channel (three tappable channels: Pink,
Orange, Purple), staying in sync across phones on "which track, how far
into it" — not on any beat grid. Protocol details:
`../docs/web-sync-protocol.md`.

## Add music

Drop `.mp3`/`.ogg`/`.wav`/`.m4a` files into `public/audio/pink/`,
`public/audio/orange/`, and/or `public/audio/purple/` — see the `README.md`
in each folder. No manifest to edit: title and duration are read
automatically from each file, and filename order (`01-...`, `02-...`)
controls playback order. An empty folder is fine; that channel just plays
silence until you add tracks. Restart the server after adding files.

## Run

```sh
npm install
npm start   # listens on :3000, override with PORT=xxxx
```

Open `http://<host>:3000/` on a phone on the same network, tap **Join the
disco**, then tap a channel swatch. The pulse circle reacts to the actual
audio (via an `AnalyserNode`), not a fixed beat. The stats panel shows your
channel, the current track title, and the estimated clock offset/RTT.
