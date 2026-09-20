# GAME.md: Bugdex (2026 Hacker Badge hack)

> Instructions for Claude: build this game as custom ESP-IDF firmware for the badge.
> `custom-firmware-hal.md` is already in your context. It is the source of truth for pins, registers, and gotchas.
> Build in the order under "Build order". After each step, stop and tell the user how to test it on real hardware.

## 1. Pitch

**Pokémon Go, but the creatures live on other hackers' badges. You can't catch one without finding the person, catching it together, and taking a selfie with them.**

Priority: engineer reasons for hackers to meet and interact. Every rule below exists to force a real, face-to-face interaction. Ties go to whichever option makes people talk more.

## 2. What the player does

1. **Find.** Every badge that is hosting a bug **flashes purple/magenta**, so you can spot the people to catch across a crowd (if another host is right next to it, only the lower-ID badge shows the flash, for demo clarity). Your badge also shows a warmer/colder meter, and its LEDs pulse faster (in amber) as you get near a host.
2. **Catch together.** Stand next to them. Both badges flash the LEDs, and both people press **A** within the window. Neither can do it alone.
3. **Selfie.** After a successful catch, the badge tells you to take a selfie together. Both badges end up with it, saved to the dex entry.
4. **Collect.** Every completed catch adds a bug + selfie + person to your dex. The goal is simply to collect as many bugs as you can.

## 3. Rules (keep it to these)

- Each badge **hosts one bug**, derived from a hash of its badge ID, so no server is needed. Use 8-10 species (`species = hash(badge_id) % N`).
- The same person can be caught **once every 30 minutes** (anti-farming). Enforce on both sides.
- There is **no score, counter, or leaderboard**. The dex is the only progress. Every completed catch is one more bug in it.
- A badge that is hosting **flashes purple/magenta** (see "Host indicator" in section 6).
- A catch only completes after the selfie is captured, or the user explicitly skips it (see section 7). The selfie is the social payoff, so make skipping a deliberate choice.

## 4. Hardware facts used (from custom-firmware-hal.md)

- ESP32-C3-MINI-1-N4, ESP-IDF v5.5.3, console over USB-Serial-JTAG.
- Screen: ST7789 320x240 RGB565 (`invert_color(true)`, `swap_xy(true)`, `mirror(true, false)`), DMA stripe buffers of ~30 rows. **Never allocate a full-frame buffer.**
- Buttons: 74HC165 shift register (A, B, Home, Down, Left, Right, Up, Aux1, all active-low) + Start on GPIO9. Poll at 10 ms and debounce.
- LEDs: 6x WS2812 on GPIO3 via RMT. Keep brightness low (AA power browns out).
- BLE: NimBLE, connectionless. Extended advertising + passive scan. Trimmed buffers. **Init once per boot, never deinit.**
- Storage partition at `0x140000`, about 1.25 MB. Use it for the dex, the met-log, and selfies.
- Accelerometer (SC7A20) and NFC are **not needed** for the core game. Do not init NFC (power, bus wedging).

## 5. Radio protocol (BLE extended advertising, manufacturer data)

All packets share a header. Multi-byte fields are little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Magic `0xB6 0xD3` |
| 2 | 1 | Version (`1`) |
| 3 | 1 | Type |
| 4 | 4 | Sender ID (low 4 bytes of the eFuse MAC) |
| 8 | n | Type-specific payload |

Types:

| Type | Name | Payload |
|---|---|---|
| `0x01` | BEACON | species (1), state (1: idle/busy) |
| `0x02` | CATCH_REQ | target_id (4), nonce (4) |
| `0x03` | CATCH_ACK | catcher_id (4), nonce (4), flash_delay_ms (2) |
| `0x04` | CATCH_RESULT | nonce (4), ok (1), press_delta_ms (2) |
| `0x05` | SELFIE_OFFER | nonce (4), thumb_size (2), total_chunks (1), thumb_crc16 (2) |
| `0x06` | SELFIE_CHUNK | nonce (4), chunk_idx (1), data (up to ~200 B) |
| `0x07` | SELFIE_ACK | nonce (4), bitmap of received chunks (up to 8 B per packet, 6 packets cover 48 chunks; send the next missing 64-chunk window) |

Rules:
- Each packet is advertised repeatedly (about 100-300 ms) and stopped or replaced when the next state begins. Dedupe by `(sender, type, nonce, chunk_idx)`.
- BEACON runs while idle at a low duty cycle. Suspend it during an encounter.
- Passive scan runs whenever the game screen is open.
- Ignore any packet whose magic or version is wrong.

## 6. State machine

```
IDLE (beacon + scan, radar on, LEDs flash purple/magenta = "I'm hosting a bug")
  |-- see BEACON with strong RSSI, press A ------> as CATCHER: send CATCH_REQ
  |-- see CATCH_REQ targeting my id -------------> as HOST: send CATCH_ACK
  v
CATCH (both badges)
  - Cue: local random delay (flash_delay_ms from ACK, 800-2000 ms), then all LEDs flash white + screen says PRESS A.
  - No timing window: ok = A was pressed at all, whenever either person gets to it. There is no
    300 ms sync requirement -- this only exists to force both people to physically engage, not to
    test reaction time.
  - Broadcast CATCH_RESULT. Success only if both results are ok. Timeout 8 s with no press -> back
    to IDLE with "try again".
  v
SELFIE (success only)
  - Catcher badge runs the selfie pipeline (section 7).
  - On completion or skip -> SAVE.
  v
SAVE
  - Write dex entry + met-log entry + selfie to flash.
  - Celebration: catch animation + LED pattern (dim!).
  v
IDLE
```

Anti-farming: before starting a CATCH, check the met-log. If that ID was caught within 30 minutes, show "already caught, come back later" and stay in IDLE. The host also refuses to ACK.

Radar: smooth RSSI with a moving average. Map it to a 5-step meter and the LED pulse rate (use amber, never purple/magenta, so the radar is not confused with another badge's host flash).

**Host indicator (purple/magenta flash):**
- While a badge is IDLE, LEDs 0-1 (the upper two) flash purple/magenta: a short pulse (about 150 ms on) every 1-1.5 s, at low brightness so it is visible across a room without draining the AA batteries.
- When the radar sees a host nearby (strong RSSI), LEDs 2-5 switch to the amber radar pulse, and LEDs 0-1 keep flashing purple/magenta.
- Demo clarity: if that nearby host is itself idle (i.e. two hosts are right next to each other), only one of the two shows the purple/magenta flash -- whichever badge has the lower badge ID. The other's LEDs 0-1 stay dark while that host is nearby (its amber radar pulse keeps working normally). Both badges are still equally catchable; this only affects what's shown on the LEDs.
- Stop the purple/magenta flash as soon as the badge enters CATCH or SELFIE (the BEACON state changes to busy), so people can see that host is taken. Resume it in IDLE. Require RSSI above a threshold (about -60 dBm, tune on site) for 2 s before CATCH_REQ is allowed, to prove the two are physically close.

## 7. Selfie pipeline (the hard part, read carefully)

Constraint: the badge has no camera, so the **phone's camera** takes the selfie. Both people must end up with it on their badge. Big BLE transfers are slow and the heap is tight, so the design is two-stage:

**Stage A: phone -> catcher's badge (Wi-Fi)**
1. Catcher's badge starts a SoftAP `BUGDEX-<last4 of ID>` (open network) and a small HTTP server, plus a DNS catch-all so the phone opens the page automatically (captive portal).
2. The badge screen shows a Wi-Fi join QR (`WIFI:T:nopass;S:BUGDEX-xxxx;;`) and the text "Join, then take a selfie together."
3. The served page (inline HTML/JS, no external requests) does:
   - `<input type="file" accept="image/*" capture="user">` to open the front camera.
   - Draws the photo to a canvas: **160x120 RGB565** (38,400 B) for the full stored copy, and **80x60 RGB565** (9,600 B) for the thumbnail. Center-crop to 4:3.
   - POSTs both as raw binary (`/full`, `/thumb`), then shows "Done!".
   - Also offers a "Save full-res photo to my phone" download so the catcher keeps the original quality.
4. The badge validates sizes, stores the 160x120 image, and keeps the 80x60 thumbnail in RAM for relay. Then it **stops Wi-Fi completely** before Stage B.

**Stage B: catcher's badge -> host's badge (BLE chunked adverts)**
1. Catcher sends SELFIE_OFFER, then SELFIE_CHUNKs of the 80x60 thumbnail (about 48 chunks of 200 B).
2. Host replies with SELFIE_ACK bitmaps. Catcher resends only missing chunks. Verify CRC16 at the end.
3. Show progress on both screens ("Sending selfie 63%"). Target under 15 s. If it stalls past 30 s, save without the selfie on the host and mark the entry "no selfie".
4. Display: the dex shows the 160x120 image at 2x on the catcher and the 80x60 thumbnail at 4x on the host. Pixelated on purpose. Tell the user the full-res photo is on the catcher's phone.

Stretch (only if everything else works): let the host phone also join the host badge's AP and upload the same photo, so both get 160x120.

Skip option: on the SELFIE screen, **B** skips. Save the catch as "no selfie". Do not silently skip.

Heap and power warnings:
- Wi-Fi + NimBLE heap use may not fit together. **Measure `esp_get_free_heap_size()` early.** Never run Wi-Fi and BLE at the same time: Stage A (Wi-Fi on, BLE beacon paused), then Wi-Fi fully off, then Stage B (BLE only).
- Wi-Fi TX spikes can brown out AA batteries. Test on fresh batteries and on USB power. Keep LEDs off during Stage A.
- If Wi-Fi is not usable on this board, fall back to: phone posts the selfie through **Web Bluetooth** (Android Chrome) in a short GATT session, still writing only 80x60 + 160x120 raw buffers. Check with the user before implementing the fallback.

## 8. Storage layout (storage partition, ~1.25 MB)

- `dex.bin`: array of entries: `{ species, person_id[4], person_name (16), timestamp, selfie_slot (0xFF = none), flags }`.
- `met.bin`: `{ person_id, last_catch_time }` for the 30-minute rule.
- `selfies`: fixed 38,400 B slots (about 30 slots; thumbnails from a partner are 9,600 B and use one slot each for simplicity, or 4 per slot if you want to pack them).
- Write with a small wear-friendly log (append + a header with a valid marker). Survive power loss without corrupting the dex.

## 9. Screens

- **Home/Radar:** the warmer/colder meter and the hosted-bug sprite when idle. No score or counters.
- **Catch:** the countdown, then a full-screen flash and "PRESS A".
- **Selfie:** join QR, status text, progress bar.
- **Dex:** grid of bug sprites, the selected one shows the selfie and the person's name.
- Sprites: 8-10 simple 32x32 RGB565 pixel bugs, original designs (do not use any Pokémon or other copyrighted characters).

Keep everything readable on the 320x240 screen. Reuse the button map: D-pad navigates, A confirms, B backs out or skips.

## 10. Build order (stop and test after each step)

1. **HAL bring-up:** buttons (HC165 + Start), screen fill test, LED chase. Follow the checklist in `custom-firmware-hal.md`.
2. **Host beacon + radar:** two badges; the meter and LED pulse react to distance.
3. **Sync catch:** the CATCH_REQ / ACK / RESULT flow and the 300 ms rule. Test with two badges.
4. **Dex, met-log, and the 30-minute rule** in flash. Verify it survives reboots.
5. **Selfie Stage A:** SoftAP + captive page + upload to the catcher's badge and display. Check heap and battery behavior.
6. **Selfie Stage B:** chunked BLE thumbnail relay with ack bitmaps.
7. **Polish:** catch animation, sprites, LED celebration.
8. **Stretch:** one raid (a laptop or spare badge broadcasts a legendary bug; 5+ badges press together), trade evolution.

## 11. Acceptance tests (two badges + one phone)

- [ ] Badge B shows a warmer/colder meter that reacts within 2 s as A walks toward it.
- [ ] Both people pressing A after the cue succeeds on both badges; a badge that never presses within the timeout fails on both.
- [ ] After a success, the catcher's phone joins the badge's Wi-Fi, takes a selfie, and the badge displays it.
- [ ] The host's badge receives the thumbnail with a valid CRC and shows it in its dex.
- [ ] An idle badge flashes purple/magenta and is visible across a room; it stops flashing during a catch and resumes afterward; if another host is right next to it, only the lower-ID badge shows the flash.
- [ ] Reboot both badges: the dex and the selfies are intact.
- [ ] Catching the same person again within 30 minutes is refused on both badges.
- [ ] Skipping the selfie with B saves a "no selfie" entry.
- [ ] Ten catches in a row without a hang, brownout, or heap exhaustion.

## 12. Open questions to ask the user before flashing

- Does re-flashing erase the event firmware (QR ID, Connect, Scanner, Sync)? There may be a supported way to add an app, or use a spare badge.
- Is Wi-Fi usable on the badge firmware, and how much free heap is left once BLE is initialized?
- Is the ~1.25 MB storage partition free for our use?
- How many badges can we test with?

## Non-goals

No server, no accounts, no NFC, no full-frame buffers, no BLE deinit, no copyrighted characters. Do not touch the event firmware's QR ID flow unless the user says how.
