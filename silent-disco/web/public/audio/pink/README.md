# Pink channel playlist

Drop audio files (`.mp3`, `.ogg`, `.wav`, or `.m4a` — mp3 has the broadest
browser support) directly into this folder. Restart the server
(`npm start`) to pick them up.

- Playback order follows filename sort order — prefix with numbers to
  control it, e.g. `01-first-song.mp3`, `02-next-song.mp3`.
- Track title is read from the file's ID3/metadata tag if present,
  otherwise falls back to the filename (with any numeric prefix stripped).
- Duration is read automatically from the file — no manual bookkeeping.
- The playlist loops forever, and phones stay in sync on the same point in
  the same track (see `docs/web-sync-protocol.md`).
- An empty folder is fine — that channel just plays silence until you add
  tracks.
