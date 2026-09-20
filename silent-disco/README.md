# Silent Disco

1. **Badge** (`badge/`): standalone LED show. Its 6 LEDs continuously
   breathe (fade bright <-> dim) in one of three colors (Pink/Orange/Purple,
   cycled locally with **Left**/**Right**). No beat sync, no clock offset,
   nothing to hear needed for the LEDs to look right.
2. **Web app** (`web/`): a phone page with a master clock; phones sync to
   it over WebSocket and play a real playlist per channel, staying in sync
   with each other on track/position (not a beat grid).
3. **Follow my badge** (Android Chrome only, optional): the badge exposes
   its current channel over a small BLE GATT service; the web app can
   connect via Web Bluetooth and switch its own channel automatically
   whenever you press Left/Right on the badge. See
   `docs/follow-badge-protocol.md`. Manual channel taps in the web app
   always work regardless of this — Web Bluetooth doesn't exist on iOS
   Safari or desktop Firefox/Safari, so this can never be required.
4. **Beacon** (`beacon/`): still a complete, working BLE beat-schedule
   broadcaster with a lookahead pattern schedule
   (steady/build-up/drop/breakdown) — see `docs/packet-format.md`. The
   badge no longer listens for it (removed along with the "Sync check"
   latency-nudge screen, which only existed to calibrate LED-vs-headphone
   timing for that beat sync). Kept around in case beat-synced rendering
   comes back later; not part of the current badge build. (Unrelated to
   Follow my badge above — that's a different BLE protocol entirely.)

See `CLAUDE.md` for the full original design and `docs/web-sync-protocol.md`
for the phone sync protocol.

## Build & flash

```sh
. ~/.espressif/tools/activate_idf_v5.5.3.sh   # puts idf.py on PATH

cd badge
idf.py set-target esp32c3
idf.py -p <port> flash monitor
```

`beacon/` (and the `beacon_packet/` wire format it shares with the old
badge code) still builds and flashes the same way, if you want to run it —
it just won't affect anything, since nothing listens for it anymore.

If flashing fails with "No serial data received", hold **Start** (GPIO9)
while plugging in USB to enter download mode -- see
`custom-firmware-hal.md` for details on that and every other hardware fact
(pins, registers, toolchain).

## What to expect

The badge's 6 LEDs continuously fade bright to dim and back (~3s cycle) in
the selected channel's color, starting immediately at boot. Press
**Left**/**Right** to cycle Pink/Orange/Purple — the screen shows the
current channel.

On **Android Chrome**, tap **Follow my badge** in the web app (hidden on
browsers without Web Bluetooth) and pick your badge by its
`SilentDisco-XXXX` name — its channel then follows your Left/Right presses
automatically. See `docs/follow-badge-protocol.md` for exactly how, plus
the memory tradeoffs involved in giving the badge a real BLE connection.

## Web app

```sh
cd web
npm install
npm start   # listens on :3000
```

See `web/README.md` for how to add music (drop files into
`web/public/audio/<color>/`, nothing to configure). Tap a color swatch
after joining to switch channels (crossfades, no resync). Ships with no
audio committed — every channel plays silence until you add tracks.
