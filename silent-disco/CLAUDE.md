# CLAUDE.md — Silent Disco Sync (Hacker Badge)

## Project summary

Silent disco for the 2026 Hacker Badge. A web app plays music to attendees' phones (headphones) while the badges act as a synchronized LED light show. Attendees pick one of 3 channels (music + LED color). **Audio never goes through the badge**: it has no audio hardware and the ESP32-C3 has BLE only, no Classic Bluetooth.

Hardware reference: `custom-firmware-hal.md` in this repo is the source of truth for pins, registers, toolchain and flashing. Do not duplicate or contradict it. Read it before touching any badge code.

## Architecture

```
Web server (master clock + track schedule)
   |-- WebSocket --> phones (Web Audio, scheduled playback)
   '-- beacon transmitter (spare badge or ESP32 board)
           '-- BLE broadcast --> badges (LED renderer)
```

- Phones and badges never connect to each other. Both follow the same master clock.
- Repo layout (create as needed):
  - `badge/` — ESP-IDF firmware or badge app (ESP32-C3)
  - `beacon/` — transmitter firmware (ESP32 board or spare badge)
  - `web/` — server + phone web app
  - `tools/` — offline beat analysis, latency measurement scripts
  - `docs/` — packet format spec, measurements

## Core design rules

1. **Broadcast a schedule, not events.** Never send "pulse now". BLE advertising has 10-30+ ms jitter and packet loss. Beacons carry channel ID, BPM, a known beat timestamp, and a short lookahead of pattern data (build-up, drop, breakdown). Badges free-run on a local timer between beacons.
2. **Clock estimation on the badge.** Estimate offset to the master clock from several beacons (favor earliest arrivals), then free-run. Lost packets must not cause visible glitches.
3. **Redundant beacons.** Send each beacon multiple times; the venue is saturated with 2.4 GHz traffic.
4. **Per-user latency offset.** Bluetooth headphones add ~150-300 ms. The badge has a "Sync check" screen: it flashes while the phone plays a click, and Up/Down nudges the LED offset in 10 ms steps. Store the offset; apply it when rendering the schedule.
5. **Pre-analyze tracks offline** (librosa/aubio) into a beat grid and energy envelope. No live beat detection from a microphone.
6. **Channel switching:** badge D-pad Left/Right changes channel and LED color instantly. Phone preloads all 3 tracks, plays them in lockstep at gain 0, and crossfades to the selected one (no resync). Live DJ streams need LEDs delayed by the stream's known latency.
7. **Channel colors:** pink, blue and green.

## Badge constraints (see custom-firmware-hal.md for details)

- ESP32-C3-MINI-1-N4, 4 MB flash, ESP-IDF v5.5.3. Console is USB-Serial-JTAG, not UART0.
- 6x WS2812B on GPIO3, GRB order, via RMT (`led_strip`). **Keep brightness modest**: full white on 6 LEDs can brown out the board on AA power.
- Buttons: 74HC165 shift register (A, B, Home, Down, Left, Right, Up, Aux1, active-low) plus Start on GPIO9. Poll ~10 ms and debounce.
- BLE via NimBLE: stock buffer sizes OOM this board. Trim to connectionless sizes (small adv buffers, few mbufs, 1M PHY only, no periodic adv). Init NimBLE **once per boot**; never deinit/reinit. Start/stop scanning instead.
- Duty-cycle BLE scanning (short window ~every second) to save battery; the free-running clock covers gaps.
- Leave NFC uninitialized; it is power-hungry and can wedge the shared I2C bus.
- Re-flashing replaces the event firmware and replacements are limited. Back up first and prefer a normal badge app if the SDK (badge.hackthenorth.com) exposes BLE scanning. Confirm that before committing to a full re-flash.
- If flashing fails with "No serial data received": hold Start (GPIO9) while plugging in USB. A blank screen in that state is download mode, not a brick.
- Turn the battery switch OFF before plugging USB into anything.

## Web app

- Server holds the master clock and per-channel track schedule; phones sync their clock over WebSocket (expect ~10-30 ms error on venue Wi-Fi).
- Playback uses Web Audio scheduled start times, not `setTimeout`.
- Phone UI channel colors must match the badge colors.
- Optional: Android Chrome "follow my badge" via Web Bluetooth. iOS Safari lacks Web Bluetooth, so it must never be required.

## Build order

1. One channel, one transmitter, fixed 120 BPM, badge pulses in time. Film slow-mo next to a phone playing the track to measure real offset.
2. Web app: clock sync and scheduled Web Audio playback.
3. Per-badge latency nudge (Sync check screen).
4. Three channels with colors, then the lookahead pattern schedule (drops, builds).

## Working agreements

- Measure, don't assume. Record inter-badge and audio-to-light offset results in `docs/measurements.md`. Targets: ~10-20 ms between badges.
- Define the beacon packet format in `docs/packet-format.md` before implementing either end; badge and beacon must share one definition.
- Keep pin numbers and register values only in `custom-firmware-hal.md`; reference them, don't copy them.
- Build in small steps and test on real hardware. Flash with `idf.py -p <port> flash` after `. ~/.espressif/tools/activate_idf_v5.5.3.sh`. Only one process may own the serial port at a time.
- Keep USB-Serial-JTAG writes under 256 bytes per chunk.

## Open questions

- Does the badge app SDK expose BLE scanning, or is a full re-flash required?
- Can one transmitter run three extended-advertising sets, or do we need three boards?
- BLE advertising vs ESP-NOW for the beacon (ESP-NOW has lower jitter but is unverified on this hardware).
