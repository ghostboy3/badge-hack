# Silent Disco

Steps 1-4 of the build order in `CLAUDE.md`. All three channels still share
one 120 BPM timeline (per-channel tempo is future work):

1. A beacon badge broadcasts a beat schedule over BLE; a receiver badge
   pulses its LEDs in time.
2. A web app holds a master clock; phones sync to it over WebSocket and
   schedule Web Audio playback.
3. The badge has a "Sync check" screen to nudge its own LED timing against
   a wearer's Bluetooth headphone latency.
4. Three channels with colors (pink/orange/purple), selected purely locally
   on each device (badge Left/Right, phone swatches -- phones and badges
   never talk to each other). The beacon also broadcasts a short lookahead
   of pattern segments (steady/build-up/drop/breakdown) computed by a fixed
   deterministic "song structure" generator (no real track/DJ input yet);
   the badge varies flash brightness, width, and sparsity by segment.

See `CLAUDE.md` for the full design, `docs/packet-format.md` for the BLE
wire format (including the pattern lookahead), and
`docs/web-sync-protocol.md` for the phone sync protocol.

## Build & flash

Two independent ESP-IDF projects, `beacon/` and `badge/`, each flashed to a
separate badge. `beacon_packet/` holds the packet format they share.

```sh
. ~/.espressif/tools/activate_idf_v5.5.3.sh   # puts idf.py on PATH

cd beacon
idf.py set-target esp32c3
idf.py -p <port> flash monitor

cd ../badge
idf.py set-target esp32c3
idf.py -p <port> flash monitor
```

Only one process may own a given serial port at a time. If flashing fails
with "No serial data received", hold **Start** (GPIO9) while plugging in
USB to enter download mode -- see `custom-firmware-hal.md` for details on
that and every other hardware fact (pins, registers, toolchain).

## What to expect

The badge's 6 LEDs start with a slow single-LED white blink ("searching").
Once it hears the beacon, they flash on the beat (120 BPM) in the selected
channel's color, with brightness/width/sparsity varying through the
~16 second steady -> build-up -> drop -> breakdown cycle. Record measured
offsets in `docs/measurements.md`.

The screen shows your channel/BPM and the current segment name. Press
**Left**/**Right** to cycle Pink/Orange/Purple. Press **Home** to enter
**Sync check**: while wearing your Bluetooth headphones and listening to
the web app's click (below), press **Up**/**Down** to nudge the LED flash
in 2 ms steps until it visually matches the click you hear. The offset is
saved to flash and re-applied on every boot; press **Home** again to exit.

## Web app

```sh
cd web
npm install
npm start   # listens on :3000
```

See `web/README.md`. Note the phone's beat grid and the beacon's beat grid
are independent anchors in step 2 -- not yet phase-aligned with each other;
see `docs/web-sync-protocol.md`'s "known gap." Tap a color swatch after
joining to switch channels (crossfades, no resync) -- there are no real
tracks yet, each channel is a distinct placeholder tone.
