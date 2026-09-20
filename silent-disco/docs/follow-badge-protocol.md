# Follow my badge (Android Chrome, Web Bluetooth)

Optional feature: the badge exposes its currently-selected channel over a
tiny BLE GATT service; a phone on Android Chrome can connect via Web
Bluetooth, subscribe, and switch its own playing channel automatically
whenever you press Left/Right on the badge. iOS Safari and desktop
Firefox/Safari have no Web Bluetooth API — manual tap-to-pick (already
built) is the always-available fallback there and everywhere else; this
feature is additive, never required, per CLAUDE.md.

This is a completely separate BLE protocol from the old beat-broadcast one
(`docs/packet-format.md`, currently unused by `badge/`) — different role
(connectable GATT peripheral vs. connectionless broadcast), different
transport, different UUIDs, no shared code.

## Identifiers

Two custom 128-bit UUIDs, generated once for this project (not standard
Bluetooth SIG UUIDs):

| | UUID |
|---|---|
| Service | `9ac33a43-5fe6-40a9-8a5f-921a1a8933e8` |
| Channel characteristic | `4e6c2458-d8ee-4401-8b35-41819926f033` |

These are hardcoded identically on both sides — `badge/main/ble_channel_service.c`
(as a reversed byte array via `BLE_UUID128_INIT`, NimBLE's convention: the
16 bytes are the UUID read right-to-left with dashes removed, e.g.
`9ac33a43-5fe6-...-...-...e8` -> last byte `0xe8` first) and
`web/public/client.js` (as the standard dashed string form, which the Web
Bluetooth API expects). There's no shared header across C and JS, so this
table is the single source of truth — if you ever change one side, change
both and update this table.

## Device advertising

- Device name: `SilentDisco-XXXX`, where `XXXX` is 2 bytes of the badge's
  own MAC address (same pattern `bugdex/main/ble_proto.c` uses for its own
  badge IDs) — needed so a venue full of badges doesn't show up as a wall
  of identical names in Chrome's device picker.
- Legacy (not extended) connectable advertising, undirected, general
  discoverable.
- The **primary advertisement packet** carries flags + the 128-bit service
  UUID; the **scan response packet** carries the device name. This split is
  required, not stylistic: flags (3 bytes) + a 128-bit UUID (18 bytes)
  already use 21 of the legacy advertisement's 31-byte budget, and
  Chrome's `requestDevice({filters: [{services: [UUID]}]})` matches only
  against UUIDs actually present in the advertisement (it does not connect
  first to check) — so the UUID has to be there, and the name has to go
  somewhere else.

## Channel characteristic

- Read + Notify, value = a single `uint8` (0 = Pink, 1 = Orange, 2 =
  Purple), matching `badge/main/main.c`'s `CHANNELS`/`s_local_channel` and
  `web/public/client.js`'s `CHANNELS`/`selectedChannel` indices.
- No pairing, no bonding, no encryption requirement on the characteristic —
  it's low-stakes (which color someone's badge is showing), and skipping
  security removes NimBLE's whole SM/crypto stack from the memory budget
  (see below). Anyone in BLE range who knows the service UUID could in
  principle read/subscribe to any badge's channel; acceptable for this
  project.
- The badge only notifies while a client is actively subscribed (tracked
  via `BLE_GAP_EVENT_SUBSCRIBE`) — it doesn't push into a vacuum.

## Memory tradeoff: connectionless vs. connectable

`custom-firmware-hal.md`'s BLE advice ("trim to connectionless sizes") was
written for the old broadcast-only design. A GATT server needs a real
connection, which is a different (and inherently somewhat larger) memory
shape. Verified against the actual installed NimBLE/controller Kconfig
(`~/esp/esp-idf/components/bt/host/nimble/Kconfig.in` and
`.../controller/esp32c3/Kconfig.in`), `badge/sdkconfig.defaults` trims it
back down as far as this use case allows:

| Config | Default | Set to | Why |
|---|---|---|---|
| `CONFIG_BT_NIMBLE_SECURITY_ENABLE` | `y` | `n` | No pairing needed; this pulls in the whole SM/crypto (mbedTLS) stack. Biggest single win. |
| `CONFIG_BT_NIMBLE_ROLE_CENTRAL` | `y` | `n` | Badge never scans/connects out. |
| `CONFIG_BT_NIMBLE_ROLE_OBSERVER` | `y` | `n` | Badge never scans. |
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | 3 | `1` | Only one phone follows one badge at a time. |
| `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` | 256 | `23` | A 1-byte value needs nothing bigger; directly sizes ACL/ATT buffers. |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | 6 | `2` | Controller-level; its own Kconfig help says ~828 bytes/instance. 2 covers 1 adv instance + 1 connection. |
| `CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` | 24 | `8` | Traffic is one infrequent 1-byte notify, not a throughput workload. |
| `CONFIG_BT_NIMBLE_EXT_ADV` | `n` (unset) | unset | Legacy advertising is simpler and sufficient for one connection; not re-enabled. |

**This is a partially-verified risk, not a guarantee.** Build success and
`idf.py size`'s static-memory report confirm the firmware links, but real
heap headroom with LVGL + display + an active BLE connection running
together can only be confirmed on real hardware. If the badge fails to
boot, resets under memory pressure, or drops the connection, the next
knobs to try (in rough order of expected impact) are: reduce
`CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` further, reduce
`display.c`'s LVGL buffer size (`buffer_size = LCD_H_RES * 40` today, per
`custom-firmware-hal.md`'s own "two ~30-row DMA stripe buffers are
plenty" advice), or drop `CONFIG_BT_CTRL_BLE_MAX_ACT` to its floor if
still reliable.

## Manual test (can't be exercised without real hardware)

1. Flash `badge/` (`idf.py -p <port> flash`).
2. Open the web app in **Android Chrome** specifically (not iOS, not
   desktop Firefox/Safari).
3. Tap **Follow my badge** — Chrome's native device picker should show
   `SilentDisco-XXXX`; pick it.
4. Press Left/Right on the badge — the phone's channel (color, playlist)
   should switch to match within about a second.
5. Manual swatch taps on the phone should still work at any time.
